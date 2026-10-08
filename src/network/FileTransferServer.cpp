#include "network/FileTransferServer.h"

#include "network/FileTransferDetail.h"

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "core/BookCache.h"
#include "core/ReadingFont.h"
#include "core/Settings.h"
#include "network/html/FileManagerPage.h"

#ifndef CROSSJP_VERSION
#define CROSSJP_VERSION "dev"
#endif

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

  if (!installUploadHandler()) {
    return false;
  }

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
    if (listed >= xfer::kMaxListEntries) {
      break;
    }
    if (file.getName(name, sizeof(name)) == 0) {
      LOG_ERR("XFER", "Skipping file with unreadable name");
      continue;
    }
    if (xfer::isProtectedPath(name) || xfer::isPartialName(name)) {
      continue;
    }
    if (!first) {
      json.push_back(',');
    }
    first = false;
    ++listed;
    json += "{\"name\":\"";
    xfer::appendJsonEscaped(json, name);
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
  if (xfer::isProtectedPath(path.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }

  HalFile file;
  if (!Storage.openFileForRead("XFER", path.c_str(), file)) {
    server->send(404, "text/plain", "File not found");
    return;
  }

  std::string header = "attachment; filename=\"";
  header += xfer::basenameOf(path.c_str());
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
    xfer::pumpNetwork();
  }
}

void FileTransferServer::handleMkdir() const {
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing name");
    return;
  }
  const std::string parent = server->hasArg("path") ? server->arg("path").c_str() : "/";
  const std::string name = server->arg("name").c_str();
  const std::string full = xfer::joinPath(parent.c_str(), name.c_str());
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
  if (xfer::isProtectedPath(oldPath.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }
  const std::string dir = xfer::dirnameOf(oldPath.c_str());
  const std::string newPath = xfer::joinPath(dir.c_str(), newName.c_str());
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
  if (xfer::isProtectedPath(oldPath.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }
  const std::string newPath = xfer::joinPath(dest.c_str(), xfer::basenameOf(oldPath.c_str()));
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
  if (xfer::isProtectedPath(path.c_str())) {
    server->send(403, "text/plain", "Protected file");
    return;
  }
  HalFile file = Storage.open(path.c_str());
  const bool isDir = file && file.isDirectory();
  file = HalFile();  // close before remove/rmdir
  bool ok = false;
  if (isDir) {
    ok = xfer::deleteDirContents(path.c_str()) && Storage.rmdir(path.c_str());
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
      xfer::appendJsonEscaped(json, name);
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
