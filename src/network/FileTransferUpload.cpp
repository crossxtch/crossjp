#include "network/FileTransferServer.h"

#include "network/FileTransferDetail.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <WebServer.h>

#include <cstring>
#include <new>

#include "core/BookCache.h"
#include "core/ReadingFont.h"
#include "core/Settings.h"

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

bool FileTransferServer::installUploadHandler() {
  uploadHandler = new (std::nothrow) RawUploadHandler(*this);
  if (!uploadHandler) {
    LOG_ERR("XFER", "OOM: upload handler");
    return false;
  }
  server->addHandler(uploadHandler);
  return true;
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
      xfer::pumpNetwork();
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
  xfer::pumpNetwork();
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
      xfer::pumpNetwork();
      delay(50);
    }
    if (upload.file.truncate(offset) && upload.file.seek64(offset)) {
      upload.file.flush();
      upload.received = offset;
      upload.preallocatedSize = 0;
      xfer::pumpNetwork();
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
  xfer::pumpNetwork();

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
  const char* leaf = xfer::basenameOf(upload.destPath.c_str());
  const size_t leafN = strlen(leaf);
  if (xfer::endsWithIgnoreCase(leaf, leafN, ".epub") && !xfer::tailHasEocd(tail, tailN)) {
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
    xfer::pumpNetwork();
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
    xfer::pumpNetwork();
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
    if (total > 0 && total <= xfer::kMaxPreallocateBytes) {
      xfer::pumpNetwork();
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
    xfer::pumpNetwork();
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
  xfer::pumpNetwork();
  return true;
}

void FileTransferServer::handleUploadStart() {
  server->client().setNoDelay(true);
  server->client().setTimeout(xfer::kUploadSocketTimeoutMs);

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
  if (!xfer::resolveUploadDest(dir.c_str(), name.c_str(), dest)) {
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
    if (!xfer::parseU64(offRaw.c_str(), offset) || !xfer::parseU64(totalRaw.c_str(), total)) {
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
    xfer::pumpNetwork();
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
    xfer::pumpNetwork();
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

  const char* leaf = xfer::basenameOf(upload.destPath.c_str());
  if (ReadingFont::isFontFilename(leaf)) {
    if (!settings.fontFile[0]) {
      ReadingFont::setActive(leaf);
    }
  } else {
    BookCache::removeFor(upload.destPath.c_str());
  }
  LOG_INF("XFER", "Uploaded %s (%lu bytes)", upload.destPath.c_str(), static_cast<unsigned long>(upload.received));
  xfer::pumpNetwork();
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
  if (!xfer::resolveUploadDest(dir.c_str(), name.c_str(), dest)) {
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
    const char* leaf = xfer::basenameOf(upload.destPath.c_str());
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
