#include "network/FileTransferServer.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "core/BookCache.h"
#include "core/ReadingFont.h"
#include "core/Settings.h"
#include "network/html/FileManagerPage.h"

#ifndef CROSSJP_VERSION
#define CROSSJP_VERSION "dev"
#endif

namespace {
// Appends `s` to `out`, escaping '"' and '\' so it stays valid inside a JSON string.
void appendJsonEscaped(std::string& out, const char* s) {
  for (const char* p = s; *p; ++p) {
    if (*p == '"' || *p == '\\') {
      out.push_back('\\');
    }
    out.push_back(*p);
  }
}

std::string joinPath(const char* dir, const char* name) {
  if (!dir || dir[0] == '\0' || (dir[0] == '/' && dir[1] == '\0')) {
    std::string out = "/";
    out += name ? name : "";
    return out;
  }
  std::string out = dir;
  if (out.back() != '/') {
    out += '/';
  }
  out += name ? name : "";
  return out;
}

// Directory portion of `path` (parent folder). "/" if path has no parent.
std::string dirnameOf(const char* path) {
  const char* slash = strrchr(path, '/');
  if (!slash || slash == path) {
    return "/";
  }
  return std::string(path, static_cast<size_t>(slash - path));
}

// Final path segment (file/folder name) of `path`.
const char* basenameOf(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

bool isProtectedPath(const char* path) {
  const char* name = basenameOf(path);
  return name[0] == '.' || strcmp(name, "System Volume Information") == 0;
}

bool isPartialName(const char* name) {
  if (!name) {
    return false;
  }
  const size_t n = strlen(name);
  constexpr char kSuffix[] = ".partial";
  constexpr size_t kLen = sizeof(kSuffix) - 1;
  return n >= kLen && memcmp(name + (n - kLen), kSuffix, kLen) == 0;
}

// Decimal only. Empty and any non-digit fail so a bad header cannot resume
// at the wrong place.
bool parseU64(const char* s, uint64_t& out) {
  if (!s || *s < '0' || *s > '9') {
    return false;
  }
  uint64_t n = 0;
  for (const char* p = s; *p; ++p) {
    if (*p < '0' || *p > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(*p - '0');
    if (n > (UINT64_MAX - digit) / 10) {
      return false;
    }
    n = n * 10 + digit;
  }
  out = n;
  return true;
}

// The macOS save panel appends ".xgf2" again when the suggested name already
// ends in it, so "Face-x3.xgf2" arrives as "Face-x3.xgf2.xgf2".
bool tailHasEocd(const uint8_t* tail, const uint32_t n) {
  if (n < 22) {
    return false;
  }
  for (int i = static_cast<int>(n) - 22; i >= 0; --i) {
    if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
      return true;
    }
  }
  return false;
}

bool endsWithIgnoreCase(const char* s, size_t n, const char* ext) {
  const size_t e = strlen(ext);
  if (n < e) {
    return false;
  }
  for (size_t i = 0; i < e; ++i) {
    char c = s[n - e + i];
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    if (c != ext[i]) {
      return false;
    }
  }
  return true;
}

// Final path for an upload. Fonts always land in /.crossjp/fonts.
bool resolveUploadDest(const char* dir, const char* name, std::string& dest) {
  if (!name || name[0] == '\0' || strchr(name, '/') || strchr(name, '\\')) {
    return false;
  }
  const char* ext = strrchr(name, '.');
  if (ext && strcasecmp(ext, ".xgf2") == 0) {
    const char* fontName = name;
    char trimmed[ReadingFont::kMaxFileName + 8];
    const size_t n = strlen(name);
    if (endsWithIgnoreCase(name, n, ".xgf2.xgf2") && n - 5 < sizeof(trimmed)) {
      memcpy(trimmed, name, n - 5);
      trimmed[n - 5] = '\0';
      fontName = trimmed;
    }
    ReadingFont::migrate();
    char fileName[ReadingFont::kMaxFileName + 1];
    if (!ReadingFont::copyFilename(fileName, sizeof(fileName), fontName)) {
      return false;
    }
    char path[192];
    ReadingFont::makePath(path, sizeof(path), fileName);
    dest = path;
    return true;
  }
  dest = joinPath(!dir || dir[0] == '\0' ? "/" : dir, name);
  return !dest.empty();
}

// WebServer::handleClient() sets a 5s socket timeout before parsing. A Wi-Fi
// hiccup makes client.readBytes() return 0, the raw parser aborts that slice,
// and the browser sends the same slice again. readBytes() also restarts this
// wait whenever any byte arrives, so a dribble stretches one slice toward a
// minute and the radio sits in that read while the retry is queued. Keep the
// quiet-gap short: the page waits slightly longer than this before resending.
constexpr uint32_t kUploadSocketTimeoutMs = 5000;
constexpr uint64_t kMaxPreallocateBytes = 2ull * 1024 * 1024;
constexpr size_t kMaxListEntries = 256;

void pumpNetwork() {
  feedLoopWDT();
  yield();
}

// Removes directory contents (including hidden files the file list hides), then
// the caller rmdirs `path`. Collects names first so we never mutate a directory
// while iterating it. Recurses only for nested folders.
bool deleteDirContents(const char* path) {
  std::vector<std::string> files;
  std::vector<std::string> dirs;
  files.reserve(32);
  dirs.reserve(8);
  {
    HalFile dir = Storage.open(path);
    if (!dir || !dir.isDirectory()) {
      return false;
    }
    auto name = makeUniqueNoThrow<char[]>(HalFile::kMaxNameBytes);
    if (!name) {
      LOG_ERR("XFER", "OOM: delete name");
      return false;
    }
    for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.getName(name.get(), HalFile::kMaxNameBytes) == 0) {
        LOG_ERR("XFER", "Unreadable name in %s", path);
        return false;
      }
      if (file.isDirectory()) {
        dirs.emplace_back(name.get());
      } else {
        files.emplace_back(name.get());
      }
    }
  }
  for (const auto& name : files) {
    pumpNetwork();
    const std::string full = joinPath(path, name.c_str());
    if (!Storage.remove(full.c_str())) {
      LOG_ERR("XFER", "Failed to remove %s", full.c_str());
      return false;
    }
    BookCache::removeFor(full.c_str());
  }
  for (const auto& name : dirs) {
    pumpNetwork();
    const std::string full = joinPath(path, name.c_str());
    if (!deleteDirContents(full.c_str()) || !Storage.rmdir(full.c_str())) {
      LOG_ERR("XFER", "Failed to remove dir %s", full.c_str());
      return false;
    }
  }
  return true;
}
}  // namespace

class FileTransferServer::RawUploadHandler : public RequestHandler {
  FileTransferServer& owner;

 public:
  explicit RawUploadHandler(FileTransferServer& owner) : owner(owner) {}

  bool canHandle(WebServer& /*server*/, HTTPMethod method, const String& uri) override {
    return method == HTTP_POST && uri == "/upload";
  }
  bool canRaw(WebServer& /*server*/, const String& uri) override { return uri == "/upload"; }

  void raw(WebServer& /*server*/, const String& /*uri*/, HTTPRaw& raw) override {
    switch (raw.status) {
      case RAW_START:
        owner.handleUploadStart();
        break;
      case RAW_WRITE:
        owner.handleUploadChunk(raw.buf, raw.currentSize);
        break;
      case RAW_END:
        owner.handleUploadEnd(raw.totalSize);
        break;
      case RAW_ABORTED:
        owner.handleUploadAbort();
        break;
    }
  }

  bool handle(WebServer& /*server*/, HTTPMethod /*method*/, const String& /*uri*/) override {
    owner.sendUploadResponse();
    return true;
  }
};

FileTransferServer::FileTransferServer() = default;
FileTransferServer::~FileTransferServer() { stop(); }

bool FileTransferServer::begin() {
  server = makeUniqueNoThrow<WebServer>(80);
  if (!server) {
    LOG_ERR("XFER", "OOM: WebServer");
    return false;
  }

  // Query-string args aren't parsed for raw-body requests; path/name travel
  // as headers. Body size is WebServer::clientContentLength().
  static const char* kCollectedHeaders[] = {"X-File-Path", "X-File-Name", "X-Upload-Offset", "X-Upload-Total"};
  server->collectHeaders(kCollectedHeaders, 4);

  uploadHandler = new (std::nothrow) RawUploadHandler(*this);
  if (!uploadHandler) {
    LOG_ERR("XFER", "OOM: upload handler");
    return false;
  }
  server->addHandler(uploadHandler);

  server->on("/", HTTP_GET, [this]() { handleRoot(); });
  server->on("/api/status", HTTP_GET, [this]() { handleStatus(); });
  server->on("/api/timezone", HTTP_POST, [this]() { handleTimezone(); });
  server->on("/api/files", HTTP_GET, [this]() { handleFileList(); });
  server->on("/api/fonts", HTTP_GET, [this]() { handleFonts(); });
  server->on("/api/fonts/select", HTTP_POST, [this]() { handleFontSelect(); });
  server->on("/api/fonts/delete", HTTP_POST, [this]() { handleFontDelete(); });
  server->on("/download", HTTP_GET, [this]() { handleDownload(); });
  server->on("/upload/cancel", HTTP_POST, [this]() { handleUploadCancel(); });
  server->on("/mkdir", HTTP_POST, [this]() { handleMkdir(); });
  server->on("/rename", HTTP_POST, [this]() { handleRename(); });
  server->on("/move", HTTP_POST, [this]() { handleMove(); });
  server->on("/delete", HTTP_POST, [this]() { handleDelete(); });
  server->onNotFound([this]() { handleNotFound(); });

  server->begin();
  running = true;

  upload.writeBuffer = makeUniqueNoThrow<uint8_t[]>(UploadState::kWriteBufferSize);
  upload.writeBufferPos = 0;

  mdnsHostname = gpio.deviceIsX3() ? "x3" : "x4";
  mdnsStarted = MDNS.begin(mdnsHostname.c_str());
  if (mdnsStarted) {
    MDNS.addService("http", "tcp", 80);
    LOG_INF("XFER", "Server started on port 80, mdns=%s.local freeHeap=%u maxAlloc=%u", mdnsHostname.c_str(),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  } else {
    LOG_ERR("XFER", "mDNS failed to start");
    mdnsHostname.clear();
    LOG_INF("XFER", "Server started on port 80, freeHeap=%u maxAlloc=%u",
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  return true;
}

void FileTransferServer::stop() {
  resetUpload(true);
  if (mdnsStarted) {
    MDNS.end();
    mdnsStarted = false;
    mdnsHostname.clear();
  }
  if (server) {
    // ~WebServer() deletes every handler registered via addHandler(),
    // including uploadHandler, so just drop our (non-owning) pointer.
    server->stop();
    server.reset();
  }
  uploadHandler = nullptr;
  running = false;
  LOG_INF("XFER", "Server stopped");
}

void FileTransferServer::handleClient() {
  if (server && WiFi.status() == WL_CONNECTED) {
    server->handleClient();
  }
}

void FileTransferServer::handleRoot() const {
  server->sendHeader("Cache-Control", "no-store");
  server->send_P(200, "text/html", FILE_MANAGER_PAGE);
}

void FileTransferServer::handleStatus() const {
  char json[256];
  snprintf(json, sizeof(json),
           "{\"version\":\"" CROSSJP_VERSION
           "\",\"ip\":\"%s\",\"ssid\":\"%s\",\"freeHeap\":%lu,\"uptime\":%lu,\"utcOffsetQ\":%u}",
           WiFi.localIP().toString().c_str(), WiFi.SSID().c_str(), static_cast<unsigned long>(ESP.getFreeHeap()),
           static_cast<unsigned long>(millis() / 1000), settings.clockUtcOffsetQ);
  server->send(200, "application/json", json);
}

void FileTransferServer::handleTimezone() {
  if (!server->hasArg("offsetQ")) {
    server->send(400, "text/plain", "Missing offsetQ");
    return;
  }
  const int q = atoi(server->arg("offsetQ").c_str());
  if (q < 0 || q > 104) {
    server->send(400, "text/plain", "offsetQ must be 0–104 (UTC−12 to UTC+14, 15 min steps)");
    return;
  }
  settings.clockUtcOffsetQ = static_cast<uint8_t>(q);
  // Manual choice wins over the one-shot HTTP timezone lookup.
  if (!settings.clockHasBeenSynced) {
    settings.clockHasBeenSynced = 1;
  }
  settings.save();
  LOG_INF("XFER", "Timezone set to q=%d", q);
  server->send(200, "text/plain", "OK");
}

void FileTransferServer::handleFileList() const {
  std::string path = server->hasArg("path") ? server->arg("path").c_str() : "/";
  if (path.empty()) {
    path = "/";
  }

  HalFile dir = Storage.open(path.c_str());
  if (!dir || !dir.isDirectory()) {
    server->send(404, "application/json", "[]");
    return;
  }

  std::string json;
  json.reserve(1024);
  json.push_back('[');
  bool first = true;
  size_t listed = 0;
  char name[HalFile::kMaxNameBytes];
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (listed >= kMaxListEntries) {
      break;
    }
    if (file.getName(name, sizeof(name)) == 0) {
      LOG_ERR("XFER", "Skipping file with unreadable name");
      continue;
    }
    if (isProtectedPath(name) || isPartialName(name)) {
      continue;
    }
    if (!first) {
      json.push_back(',');
    }
    first = false;
    ++listed;
    json += "{\"name\":\"";
    appendJsonEscaped(json, name);
    json += "\",\"size\":";
    json += std::to_string(file.isDirectory() ? 0 : file.fileSize());
    json += ",\"isDirectory\":";
    json += file.isDirectory() ? "true" : "false";
    json += "}";
  }
  json.push_back(']');
  server->send(200, "application/json", json.c_str());
}

void FileTransferServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }
  const std::string path = server->arg("path").c_str();
  if (isProtectedPath(path.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("XFER", path.c_str(), file)) {
    server->send(404, "text/plain", "File not found");
    return;
  }

  std::string header = "attachment; filename=\"";
  header += basenameOf(path.c_str());
  header += '"';
  server->sendHeader("Content-Disposition", header.c_str());
  server->setContentLength(file.fileSize());
  server->send(200, "application/octet-stream", "");
  server->client().setNoDelay(true);

  constexpr size_t kChunkSize = 16384;
  auto buffer = makeUniqueNoThrow<uint8_t[]>(kChunkSize);
  if (!buffer) {
    LOG_ERR("XFER", "OOM: download buffer");
    return;
  }
  int n;
  while ((n = file.read(buffer.get(), kChunkSize)) > 0) {
    if (!server->client().connected()) {
      break;
    }
    server->client().write(buffer.get(), static_cast<size_t>(n));
    pumpNetwork();
  }
}

void FileTransferServer::writeUploadBytes(const uint8_t* data, size_t len) {
  if (!upload.success || !upload.file || !len) {
    return;
  }
  // A failed FatFile::write returns 0 even when it already advanced the file
  // position. Remember the size from before the call and seek back to retry.
  // The card is often still inside a multi-block write; stop that before the
  // seek, or the retry times out the same way.
  const uint64_t at = upload.file.fileSize64();
  upload.lastWriteMs = 0;
  for (int attempt = 1; attempt <= 2; ++attempt) {
    if (attempt > 1) {
      uint8_t sdErr = 0;
      Storage.recoverCard(&sdErr);
      LOG_ERR("XFER", "SD write err 0x%02X", sdErr);
      upload.file.clearWriteError();
      pumpNetwork();
      if (!upload.file.seek64(at)) {
        LOG_ERR("XFER", "Could not retry SD write at %lu", static_cast<unsigned long>(at));
        upload.success = false;
        upload.statusCode = 503;
        return;
      }
    }
    const uint32_t startedMs = millis();
    const size_t written = upload.file.write(data, len);
    const uint32_t tookMs = millis() - startedMs;
    if (tookMs > upload.lastWriteMs) {
      upload.lastWriteMs = tookMs;
    }
    if (written == len) {
      return;
    }
    LOG_ERR("XFER", "Short upload write (%u of %u) at %lu attempt %d", static_cast<unsigned>(written),
            static_cast<unsigned>(len), static_cast<unsigned long>(at), attempt);
  }
  Storage.recoverCard(nullptr);
  upload.success = false;
  upload.statusCode = 503;
}

void FileTransferServer::resetUpload(bool removePartial) {
  upload.writeBufferPos = 0;
  upload.preallocatedSize = 0;
  upload.received = 0;
  upload.chunkGot = 0;
  upload.chunkExpected = 0;
  upload.chunkOffset = 0;
  upload.totalSize = 0;
  upload.chunkOpen = false;
  upload.lastChunk = false;
  upload.success = false;
  upload.statusCode = 500;
  const std::string partial = upload.partialPath;
  Storage.recoverCard(nullptr);
  upload.file = HalFile();
  if (removePartial && !partial.empty()) {
    Storage.remove(partial.c_str());
    upload.partialPath.clear();
    upload.destPath.clear();
  }
}

void FileTransferServer::flushWriteBuffer() {
  if (upload.writeBufferPos == 0 || !upload.writeBuffer) {
    return;
  }
  writeUploadBytes(upload.writeBuffer.get(), upload.writeBufferPos);
  upload.writeBufferPos = 0;
  // The SD write just ran with lwIP blocked. Let the radio ACK before the next one.
  pumpNetwork();
}

bool FileTransferServer::rewindPartial(uint64_t offset) {
  if (!upload.file) {
    return false;
  }
  for (int attempt = 1; attempt <= 3; ++attempt) {
    uint8_t sdErr = 0;
    Storage.recoverCard(&sdErr);
    upload.file.clearWriteError();
    if (attempt > 1) {
      pumpNetwork();
      delay(50);
    }
    if (upload.file.truncate(offset) && upload.file.seek64(offset)) {
      upload.file.flush();
      upload.received = offset;
      upload.preallocatedSize = 0;
      pumpNetwork();
      return true;
    }
    LOG_ERR("XFER", "Could not rewind partial to %lu attempt %d (sd 0x%02X)", static_cast<unsigned long>(offset),
            attempt, sdErr);
  }
  // Keep the handle and the prefix length. Closing now would let SdFat mark
  // itself idle while the card is still in the multi-block write, and the
  // resend would then treat a failed directory read as a missing partial.
  upload.preallocatedSize = 0;
  return false;
}

bool FileTransferServer::commitPartial() {
  const std::string partial = upload.partialPath;
  const uint64_t expect = upload.received;
  auto reject = [&](const char* why) {
    LOG_ERR("XFER", "%s %s (%lu bytes)", why, partial.c_str(), static_cast<unsigned long>(expect));
    Storage.recoverCard(nullptr);
    upload.file = HalFile();
    Storage.remove(partial.c_str());
    upload.partialPath.clear();
    upload.success = false;
    upload.statusCode = 409;
  };
  if (!upload.file.sync()) {
    reject("Could not sync");
    return false;
  }
  upload.file = HalFile();
  pumpNetwork();

  HalFile check;
  if (!Storage.openFileForRead("XFER", partial.c_str(), check)) {
    reject("Could not reopen");
    return false;
  }
  const uint64_t sz = check.fileSize64();
  if (sz != expect) {
    LOG_ERR("XFER", "Partial size %lu, expected %lu", static_cast<unsigned long>(sz),
            static_cast<unsigned long>(expect));
    check = HalFile();
    reject("Size mismatch");
    return false;
  }
  std::unique_ptr<uint8_t[]> tailBuf;
  uint8_t* tail = upload.writeBuffer.get();
  if (!tail) {
    tailBuf = makeUniqueNoThrow<uint8_t[]>(1024);
    tail = tailBuf.get();
  }
  if (!tail) {
    check = HalFile();
    reject("OOM");
    return false;
  }
  const uint32_t tailN = sz < 1024 ? static_cast<uint32_t>(sz) : 1024;
  const uint64_t tailOff = sz - tailN;
  bool got = check.seek64(tailOff) && check.read(tail, tailN) == static_cast<int>(tailN);
  if (!got) {
    Storage.recoverCard(nullptr);
    check = HalFile();
    if (!Storage.openFileForRead("XFER", partial.c_str(), check)) {
      reject("Could not reread");
      return false;
    }
    got = check.seek64(tailOff) && check.read(tail, tailN) == static_cast<int>(tailN);
  }
  if (!got) {
    check = HalFile();
    reject("End unreadable");
    return false;
  }
  const char* leaf = basenameOf(upload.destPath.c_str());
  const size_t leafN = strlen(leaf);
  if (endsWithIgnoreCase(leaf, leafN, ".epub") && !tailHasEocd(tail, tailN)) {
    check = HalFile();
    reject("EPUB has no end marker");
    return false;
  }
  check = HalFile();
  return true;
}

bool FileTransferServer::rollbackChunk() {
  upload.writeBufferPos = 0;
  upload.chunkGot = 0;
  if (!upload.file) {
    upload.received = upload.chunkOffset;
    upload.preallocatedSize = 0;
    return true;
  }
  return rewindPartial(upload.chunkOffset);
}

bool FileTransferServer::openPartial(const std::string& dest, uint64_t offset, uint64_t total, uint64_t chunkLen) {
  const std::string partial = dest + ".partial";
  if (upload.file && upload.partialPath != partial) {
    Storage.recoverCard(nullptr);
    upload.file.flush();
    pumpNetwork();
    upload.file = HalFile();
    upload.received = 0;
    upload.preallocatedSize = 0;
  }

  if (offset == 0) {
    if (upload.file) {
      Storage.recoverCard(nullptr);
      upload.file = HalFile();
    }
    upload.received = 0;
    upload.preallocatedSize = 0;
    pumpNetwork();
    if (!Storage.openFileForWrite("XFER", partial.c_str(), upload.file)) {
      uint8_t sdErr = 0;
      Storage.recoverCard(&sdErr);
      if (!Storage.openFileForWrite("XFER", partial.c_str(), upload.file)) {
        LOG_ERR("XFER", "Failed to create %s (sd 0x%02X)", partial.c_str(), sdErr);
        upload.destPath = dest;
        upload.partialPath = partial;
        upload.statusCode = 500;
        return false;
      }
    }
    // A full-file preAllocate of a large book or font stalls SPI long enough
    // to leave the card mid-FAT-update. Only small files get one extent.
    if (total > 0 && total <= kMaxPreallocateBytes) {
      pumpNetwork();
      if (upload.file.preAllocate(total)) {
        upload.preallocatedSize = total;
        LOG_INF("XFER", "preAllocate %lu bytes", static_cast<unsigned long>(total));
      } else {
        LOG_ERR("XFER", "preAllocate %lu failed", static_cast<unsigned long>(total));
      }
    }
  } else if (upload.file && upload.partialPath == partial && upload.received == offset) {
    // A no-op when the handle is already there. seekSet rejects past EOF.
    if (!upload.file.seek64(offset)) {
      Storage.recoverCard(nullptr);
      if (!upload.file.seek64(offset)) {
        LOG_ERR("XFER", "Could not seek %s", partial.c_str());
        upload.statusCode = 503;
        return false;
      }
    }
  } else if (upload.file && upload.partialPath == partial && offset < upload.received) {
    // The browser did not see the previous 200 and is rewriting this slice.
    if (!rewindPartial(offset)) {
      upload.statusCode = 503;
      return false;
    }
  } else if (upload.file && upload.partialPath == partial) {
    LOG_ERR("XFER", "Upload gap off=%lu have=%lu", static_cast<unsigned long>(offset),
            static_cast<unsigned long>(upload.received));
    upload.statusCode = 409;
    return false;
  } else {
    if (upload.file) {
      Storage.recoverCard(nullptr);
      upload.file = HalFile();
    }
    upload.received = 0;
    upload.preallocatedSize = 0;
    // Stop a stuck multi-block write before the directory read. A wedged card
    // fails that read, which looks like a missing partial and makes the page
    // throw away the prefix.
    uint8_t sdErr = 0;
    Storage.recoverCard(&sdErr);
    if (!Storage.exists(partial.c_str())) {
      LOG_ERR("XFER", "No partial for %s at %lu (sd 0x%02X)", dest.c_str(), static_cast<unsigned long>(offset),
              sdErr);
      upload.destPath = dest;
      upload.partialPath = partial;
      upload.statusCode = 409;
      return false;
    }
    upload.file = Storage.open(partial.c_str(), O_RDWR);
    if (!upload.file) {
      Storage.recoverCard(nullptr);
      upload.file = Storage.open(partial.c_str(), O_RDWR);
    }
    if (!upload.file) {
      LOG_ERR("XFER", "Failed to open %s", partial.c_str());
      upload.destPath = dest;
      upload.partialPath = partial;
      upload.statusCode = 500;
      return false;
    }
    const uint64_t sz = upload.file.fileSize64();
    if (offset > sz) {
      upload.file = HalFile();
      LOG_ERR("XFER", "Partial shorter than %lu", static_cast<unsigned long>(offset));
      upload.destPath = dest;
      upload.partialPath = partial;
      upload.statusCode = 409;
      return false;
    }
    if (offset < sz) {
      if (!rewindPartial(offset)) {
        upload.destPath = dest;
        upload.partialPath = partial;
        upload.statusCode = 503;
        return false;
      }
    } else if (!upload.file.seek64(offset)) {
      Storage.recoverCard(nullptr);
      if (!upload.file.seek64(offset)) {
        LOG_ERR("XFER", "Could not seek %s", partial.c_str());
        upload.destPath = dest;
        upload.partialPath = partial;
        upload.statusCode = 503;
        return false;
      }
    }
    upload.received = offset;
    pumpNetwork();
  }

  upload.destPath = dest;
  upload.partialPath = partial;
  upload.totalSize = total;
  upload.chunkOffset = offset;
  upload.chunkExpected = chunkLen;
  upload.chunkGot = 0;
  upload.writeBufferPos = 0;
  upload.lastChunk = offset + chunkLen >= total;
  upload.success = true;
  upload.statusCode = 200;
  upload.chunkOpen = true;
  pumpNetwork();
  return true;
}

void FileTransferServer::handleUploadStart() {
  server->client().setNoDelay(true);
  server->client().setTimeout(kUploadSocketTimeoutMs);

  upload.success = false;
  upload.chunkOpen = false;
  upload.chunkGot = 0;
  upload.writeBufferPos = 0;
  upload.statusCode = 500;

  const std::string dir = WebServer::urlDecode(server->header("X-File-Path")).c_str();
  const std::string name = WebServer::urlDecode(server->header("X-File-Name")).c_str();
  if (name.empty()) {
    LOG_ERR("XFER", "Upload missing X-File-Name header");
    upload.statusCode = 400;
    return;
  }
  std::string dest;
  if (!resolveUploadDest(dir.c_str(), name.c_str(), dest)) {
    LOG_ERR("XFER", "Bad upload name");
    upload.statusCode = 400;
    return;
  }

  const String offRaw = server->header("X-Upload-Offset");
  const String totalRaw = server->header("X-Upload-Total");
  const bool hasOff = offRaw.length() > 0;
  const bool hasTotal = totalRaw.length() > 0;
  const int contentLength = server->clientContentLength();
  if (contentLength < 0 || hasOff != hasTotal) {
    LOG_ERR("XFER", "Bad upload headers");
    upload.statusCode = 400;
    return;
  }
  uint64_t offset = 0;
  uint64_t total = static_cast<uint64_t>(contentLength);
  if (hasOff) {
    if (!parseU64(offRaw.c_str(), offset) || !parseU64(totalRaw.c_str(), total)) {
      LOG_ERR("XFER", "Bad upload range");
      upload.statusCode = 400;
      return;
    }
  }
  const uint64_t chunkLen = static_cast<uint64_t>(contentLength);
  if (chunkLen > UINT64_MAX - offset || offset + chunkLen > total) {
    LOG_ERR("XFER", "Upload range past end");
    upload.statusCode = 400;
    return;
  }
  if (!openPartial(dest, offset, total, chunkLen)) {
    return;
  }
  LOG_INF("XFER", "Upload %s off=%lu len=%lu total=%lu", upload.destPath.c_str(), static_cast<unsigned long>(offset),
          static_cast<unsigned long>(chunkLen), static_cast<unsigned long>(total));
}

void FileTransferServer::handleUploadChunk(const uint8_t* data, size_t len) {
  if (!upload.chunkOpen || !upload.success || !upload.file || !len) {
    return;
  }
  if (upload.chunkGot > upload.chunkExpected || len > upload.chunkExpected - upload.chunkGot) {
    LOG_ERR("XFER", "Upload chunk exceeds Content-Length");
    upload.success = false;
    upload.statusCode = 400;
    return;
  }
  if (!upload.writeBuffer) {
    writeUploadBytes(data, len);
    upload.chunkGot += len;
    pumpNetwork();
    return;
  }
  size_t offset = 0;
  while (offset < len) {
    const size_t space = UploadState::kWriteBufferSize - upload.writeBufferPos;
    const size_t chunk = std::min(space, len - offset);
    memcpy(upload.writeBuffer.get() + upload.writeBufferPos, data + offset, chunk);
    upload.writeBufferPos += chunk;
    offset += chunk;
    if (upload.writeBufferPos == UploadState::kWriteBufferSize) {
      flushWriteBuffer();
      if (!upload.success) {
        return;
      }
    }
  }
  upload.chunkGot += len;
}

void FileTransferServer::handleUploadEnd(size_t totalBytes) {
  if (!upload.chunkOpen) {
    return;
  }
  if (upload.success) {
    flushWriteBuffer();
  }
  if (upload.success && (upload.chunkGot != upload.chunkExpected ||
                         static_cast<uint64_t>(totalBytes) != upload.chunkExpected)) {
    LOG_ERR("XFER", "Short upload body");
    upload.success = false;
  }
  if (!upload.success) {
    if (upload.statusCode == 200) {
      upload.statusCode = 500;
    }
    const bool rewound = rollbackChunk();
    // freeClusterCount walks the whole FAT with no yield. A short write that
    // returned in under two seconds is a stuck transfer, not a full card.
    if (upload.statusCode == 503 && rewound && upload.lastWriteMs >= 2000) {
      const uint64_t need = upload.totalSize > upload.chunkOffset ? upload.totalSize - upload.chunkOffset : 0;
      uint64_t freeB = 0;
      if (Storage.freeBytes(&freeB)) {
        LOG_ERR("XFER", "SD free %lu KB, need %lu KB", static_cast<unsigned long>(freeB / 1024),
                static_cast<unsigned long>(need / 1024));
        if (freeB < need) {
          upload.statusCode = 507;
        }
      } else {
        LOG_ERR("XFER", "Could not read SD free space");
      }
    }
    if (upload.statusCode == 503) {
      LOG_INF("XFER", "Page will resend slice at %lu", static_cast<unsigned long>(upload.chunkOffset));
    }
    upload.chunkOpen = false;
    return;
  }

  upload.received = upload.chunkOffset + upload.chunkGot;
  upload.chunkOpen = false;
  if (!upload.lastChunk) {
    upload.file.flush();
    pumpNetwork();
    LOG_INF("XFER", "Chunk %lu/%lu %s", static_cast<unsigned long>(upload.received),
            static_cast<unsigned long>(upload.totalSize), upload.destPath.c_str());
    return;
  }
  if (upload.received != upload.totalSize) {
    LOG_ERR("XFER", "Upload ended at %lu of %lu", static_cast<unsigned long>(upload.received),
            static_cast<unsigned long>(upload.totalSize));
    upload.success = false;
    upload.statusCode = 500;
    return;
  }
  if (upload.preallocatedSize > upload.received && !upload.file.truncate(upload.received)) {
    LOG_ERR("XFER", "Could not trim %s", upload.partialPath.c_str());
    upload.success = false;
    upload.statusCode = 500;
    return;
  }
  // A write() that returned can still leave a directory size the card cannot
  // seek. Do not rename until the end of the partial reads back.
  if (!commitPartial()) {
    return;
  }

  if (Storage.exists(upload.destPath.c_str()) && !Storage.remove(upload.destPath.c_str())) {
    LOG_ERR("XFER", "Could not replace %s", upload.destPath.c_str());
    upload.success = false;
    upload.statusCode = 500;
    return;
  }
  if (!Storage.rename(upload.partialPath.c_str(), upload.destPath.c_str())) {
    LOG_ERR("XFER", "Could not rename %s", upload.partialPath.c_str());
    upload.success = false;
    upload.statusCode = 500;
    return;
  }
  upload.partialPath.clear();
  upload.preallocatedSize = 0;

  const char* leaf = basenameOf(upload.destPath.c_str());
  if (ReadingFont::isFontFilename(leaf)) {
    if (!settings.fontFile[0]) {
      ReadingFont::setActive(leaf);
    }
  } else {
    BookCache::removeFor(upload.destPath.c_str());
  }
  LOG_INF("XFER", "Uploaded %s (%lu bytes)", upload.destPath.c_str(), static_cast<unsigned long>(upload.received));
  pumpNetwork();
}

void FileTransferServer::handleUploadAbort() {
  LOG_ERR("XFER", "Slice stalled at %lu of %s (free %u), partial kept",
          static_cast<unsigned long>(upload.chunkOffset), upload.destPath.c_str(),
          static_cast<unsigned>(ESP.getFreeHeap()));
  if (!upload.chunkOpen) {
    upload.success = false;
    return;
  }
  rollbackChunk();
  upload.chunkOpen = false;
  upload.success = false;
  if (upload.statusCode == 200) {
    upload.statusCode = 500;
  }
}

void FileTransferServer::handleUploadCancel() {
  const std::string dir = server->hasArg("path") ? server->arg("path").c_str() : "";
  const std::string name = server->hasArg("name") ? server->arg("name").c_str() : "";
  std::string dest;
  if (!resolveUploadDest(dir.c_str(), name.c_str(), dest)) {
    server->send(400, "text/plain", "Bad path");
    return;
  }
  const std::string partial = dest + ".partial";
  if (upload.partialPath == partial) {
    resetUpload(true);
  } else {
    Storage.remove(partial.c_str());
  }
  server->send(200, "text/plain", "OK");
}

void FileTransferServer::sendUploadResponse() const {
  server->sendHeader("Connection", "close");
  if (upload.success) {
    if (!upload.lastChunk) {
      server->send(200, "text/plain", "OK");
      return;
    }
    std::string msg;
    const char* leaf = basenameOf(upload.destPath.c_str());
    if (ReadingFont::isFontFilename(leaf)) {
      msg = "Font installed: ";
      msg += leaf;
    } else {
      msg = "File uploaded successfully: ";
      msg += leaf;
    }
    server->send(200, "text/plain", msg.c_str());
    return;
  }
  // 503: the slice failed and the prefix is still on the card. Dropping the
  // connection makes the page resend this slice. A 500 would stop the upload.
  if (upload.statusCode == 503) {
    server->client().stop();
    return;
  }
  const int code = (upload.statusCode == 400 || upload.statusCode == 409 || upload.statusCode == 507)
                       ? upload.statusCode
                       : 500;
  const char* msg = "Upload failed";
  if (code == 409) {
    msg = "Upload restart";
  } else if (code == 400) {
    msg = "Bad upload";
  } else if (code == 507) {
    msg = "SD card full";
  }
  server->send(code, "text/plain", msg);
}

void FileTransferServer::handleMkdir() const {
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing name");
    return;
  }
  const std::string parent = server->hasArg("path") ? server->arg("path").c_str() : "/";
  const std::string name = server->arg("name").c_str();
  const std::string full = joinPath(parent.c_str(), name.c_str());
  if (Storage.mkdir(full.c_str())) {
    server->send(200, "text/plain", "OK");
  } else {
    server->send(500, "text/plain", "Could not create folder");
  }
}

void FileTransferServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or name");
    return;
  }
  const std::string oldPath = server->arg("path").c_str();
  const std::string newName = server->arg("name").c_str();
  if (isProtectedPath(oldPath.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }
  const std::string dir = dirnameOf(oldPath.c_str());
  const std::string newPath = joinPath(dir.c_str(), newName.c_str());
  if (Storage.rename(oldPath.c_str(), newPath.c_str())) {
    BookCache::removeFor(oldPath.c_str());
    server->send(200, "text/plain", "OK");
  } else {
    server->send(500, "text/plain", "Rename failed");
  }
}

void FileTransferServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or dest");
    return;
  }
  const std::string oldPath = server->arg("path").c_str();
  const std::string dest = server->arg("dest").c_str();
  if (isProtectedPath(oldPath.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }
  const std::string newPath = joinPath(dest.c_str(), basenameOf(oldPath.c_str()));
  if (Storage.rename(oldPath.c_str(), newPath.c_str())) {
    BookCache::removeFor(oldPath.c_str());
    server->send(200, "text/plain", "OK");
  } else {
    server->send(500, "text/plain", "Move failed");
  }
}

void FileTransferServer::handleDelete() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }
  const std::string path = server->arg("path").c_str();
  if (path.empty() || path == "/") {
    server->send(403, "text/plain", "Cannot delete root");
    return;
  }
  if (isProtectedPath(path.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }
  HalFile file = Storage.open(path.c_str());
  const bool isDir = file && file.isDirectory();
  file = HalFile();  // close before remove/rmdir
  bool ok = false;
  if (isDir) {
    ok = deleteDirContents(path.c_str()) && Storage.rmdir(path.c_str());
  } else if (Storage.remove(path.c_str())) {
    BookCache::removeFor(path.c_str());
    ok = true;
  }
  if (ok) {
    LOG_INF("XFER", "Deleted %s", path.c_str());
    server->send(200, "text/plain", "OK");
  } else {
    LOG_ERR("XFER", "Delete failed: %s", path.c_str());
    server->send(500, "text/plain", isDir ? "Could not delete folder" : "Delete failed");
  }
}

void FileTransferServer::handleFonts() const {
  ReadingFont::migrate();
  HalFile dir = Storage.open(ReadingFont::kDir);
  std::string json;
  json.reserve(512);
  json.push_back('[');
  bool first = true;
  if (dir && dir.isDirectory()) {
    char name[HalFile::kMaxNameBytes];
    size_t listed = 0;
    for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (listed >= 32) {
        break;
      }
      if (file.isDirectory() || file.getName(name, sizeof(name)) == 0 || !ReadingFont::isFontFilename(name)) {
        continue;
      }
      if (!first) {
        json.push_back(',');
      }
      first = false;
      ++listed;
      json += "{\"name\":\"";
      appendJsonEscaped(json, name);
      json += "\",\"size\":";
      json += std::to_string(file.fileSize());
      json += ",\"active\":";
      json += (settings.fontFile[0] && strcmp(name, settings.fontFile) == 0) ? "true" : "false";
      json += "}";
    }
  }
  json.push_back(']');
  server->send(200, "application/json", json.c_str());
}

void FileTransferServer::handleFontSelect() {
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing name");
    return;
  }
  if (!ReadingFont::setActive(server->arg("name").c_str())) {
    server->send(404, "text/plain", "Font not found");
    return;
  }
  server->send(200, "text/plain", "OK");
}

void FileTransferServer::handleFontDelete() {
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing name");
    return;
  }
  char fileName[ReadingFont::kMaxFileName + 1];
  if (!ReadingFont::copyFilename(fileName, sizeof(fileName), server->arg("name").c_str())) {
    server->send(400, "text/plain", "Bad name");
    return;
  }
  char path[192];
  ReadingFont::makePath(path, sizeof(path), fileName);
  if (!Storage.exists(path)) {
    server->send(404, "text/plain", "Font not found");
    return;
  }
  if (!Storage.remove(path)) {
    server->send(500, "text/plain", "Delete failed");
    return;
  }
  if (strcmp(settings.fontFile, fileName) == 0) {
    settings.fontFile[0] = '\0';
    settings.save();
  }
  LOG_INF("XFER", "Deleted font %s", fileName);
  server->send(200, "text/plain", "OK");
}

void FileTransferServer::handleNotFound() const { server->send(404, "text/plain", "Not found"); }
