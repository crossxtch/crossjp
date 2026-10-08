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

// WebServer::handleClient() sets a 5s socket timeout before parsing. That's
// too tight for a book upload: a Wi-Fi hiccup or a blocking SD write on the
// single-core C3 makes client.readBytes() return 0, the raw parser aborts,
// and Chrome logs net::ERR_CONNECTION_RESET. Per-chunk, not overall. Keep
// this short enough that a dead peer unblocks the UI (Back) within seconds.
constexpr uint32_t kUploadSocketTimeoutMs = 20000;
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
  static const char* kCollectedHeaders[] = {"X-File-Path", "X-File-Name"};
  server->collectHeaders(kCollectedHeaders, 2);

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

void FileTransferServer::handleRoot() const { server->send_P(200, "text/html", FILE_MANAGER_PAGE); }

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
    if (isProtectedPath(name)) {
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
  const size_t written = upload.file.write(data, len);
  feedLoopWDT();
  if (written != len) {
    LOG_ERR("XFER", "Short upload write (%u of %u)", static_cast<unsigned>(written), static_cast<unsigned>(len));
    upload.success = false;
  }
}

void FileTransferServer::resetUpload(bool removePartial) {
  upload.writeBufferPos = 0;
  upload.preallocatedSize = 0;
  const std::string path = upload.destPath;
  const bool hadFile = static_cast<bool>(upload.file);
  upload.file = HalFile();
  upload.success = false;
  if (removePartial && hadFile && !path.empty()) {
    Storage.remove(path.c_str());
  }
}

void FileTransferServer::flushWriteBuffer() {
  if (upload.writeBufferPos == 0 || !upload.writeBuffer) {
    return;
  }
  writeUploadBytes(upload.writeBuffer.get(), upload.writeBufferPos);
  upload.writeBufferPos = 0;
}

void FileTransferServer::handleUploadStart() {
  resetUpload(true);
  server->client().setNoDelay(true);
  server->client().setTimeout(kUploadSocketTimeoutMs);

  const std::string dir = WebServer::urlDecode(server->header("X-File-Path")).c_str();
  const std::string name = WebServer::urlDecode(server->header("X-File-Name")).c_str();
  if (name.empty()) {
    LOG_ERR("XFER", "Upload missing X-File-Name header");
    return;
  }
  const char* ext = strrchr(name.c_str(), '.');
  const bool isFont = ext && strcasecmp(ext, ".xgf2") == 0;
  if (isFont) {
    ReadingFont::migrate();
    char fileName[ReadingFont::kMaxFileName + 1];
    if (!ReadingFont::copyFilename(fileName, sizeof(fileName), name.c_str())) {
      LOG_ERR("XFER", "Bad font name");
      return;
    }
    char dest[192];
    ReadingFont::makePath(dest, sizeof(dest), fileName);
    upload.destPath = dest;
  } else {
    upload.destPath = joinPath(dir.empty() ? "/" : dir.c_str(), name.c_str());
  }
  // FAT open/preAllocate can block; yield so lwIP can ACK bytes already in flight.
  pumpNetwork();
  if (!Storage.openFileForWrite("XFER", upload.destPath.c_str(), upload.file)) {
    LOG_ERR("XFER", "Failed to create %s", upload.destPath.c_str());
    return;
  }
  upload.success = true;

  const int contentLength = server->clientContentLength();
  // Full-file preAllocate of a large book can stall SPI long enough to trip
  // the task WDT and leave the card mid-FAT-update (needs a power cycle).
  if (contentLength > 0 && static_cast<uint64_t>(contentLength) <= kMaxPreallocateBytes) {
    pumpNetwork();
    if (upload.file.preAllocate(static_cast<uint64_t>(contentLength))) {
      upload.preallocatedSize = static_cast<uint64_t>(contentLength);
      LOG_INF("XFER", "preAllocate %d bytes", contentLength);
    } else {
      LOG_ERR("XFER", "preAllocate %d failed", contentLength);
    }
  }
  pumpNetwork();
}

void FileTransferServer::handleUploadChunk(const uint8_t* data, size_t len) {
  if (!upload.success || !upload.file || !len) {
    return;
  }
  if (!upload.writeBuffer) {
    writeUploadBytes(data, len);
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
}

void FileTransferServer::handleUploadEnd(size_t totalBytes) {
  if (upload.file) {
    flushWriteBuffer();
    if (upload.success && upload.preallocatedSize > totalBytes) {
      upload.file.truncate(totalBytes);
    }
    upload.file.flush();
    upload.file.close();
    upload.file = HalFile();
  }
  if (upload.success) {
    const char* leaf = basenameOf(upload.destPath.c_str());
    if (ReadingFont::isFontFilename(leaf)) {
      if (!settings.fontFile[0]) {
        ReadingFont::setActive(leaf);
      }
    } else {
      BookCache::removeFor(upload.destPath.c_str());
    }
    LOG_INF("XFER", "Uploaded %s (%lu bytes)", upload.destPath.c_str(), static_cast<unsigned long>(totalBytes));
  } else if (!upload.destPath.empty()) {
    Storage.remove(upload.destPath.c_str());
    LOG_ERR("XFER", "Upload failed, removed %s", upload.destPath.c_str());
  }
  pumpNetwork();
}

void FileTransferServer::handleUploadAbort() {
  LOG_ERR("XFER", "Upload aborted: %s", upload.destPath.c_str());
  resetUpload(true);
}

void FileTransferServer::sendUploadResponse() const {
  server->sendHeader("Connection", "close");
  if (upload.success) {
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
  } else {
    server->send(500, "text/plain", "Upload failed");
  }
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
