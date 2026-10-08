#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>
#include <string>

class WebServer;

// HTTP-only (no WebSocket/WebDAV) file transfer server: serves a small static
// file manager page and a handful of JSON/file endpoints against the SD card.
class FileTransferServer {
 public:
  FileTransferServer();
  ~FileTransferServer();

  // Starts listening on port 80. Call once Wi-Fi is connected. Advertises
  // "x3.local"/"x4.local" via mDNS depending on the running device.
  bool begin();
  void stop();
  void handleClient();
  bool isRunning() const { return running; }
  // Empty if mDNS didn't start (e.g. begin() not called or it failed).
  const std::string& hostname() const { return mdnsHostname; }

 private:
  std::unique_ptr<WebServer> server;
  bool running = false;
  bool mdnsStarted = false;
  std::string mdnsHostname;

  // One upload at a time. The page posts 1 MB slices (X-Upload-Offset /
  // X-Upload-Total) into dest + ".partial"; the last slice renames it into
  // place. A request without those headers is the whole file (offset 0).
  // The raw parser hands us 1436-byte buffers. Those are coalesced into 4 KB
  // SD writes. A longer SPI write on this single core stops lwIP from ACKing.
  struct UploadState {
    HalFile file;
    std::string destPath;
    std::string partialPath;
    bool success = false;
    bool chunkOpen = false;
    bool lastChunk = false;
    int statusCode = 500;
    uint64_t totalSize = 0;
    uint64_t received = 0;
    uint64_t chunkOffset = 0;
    uint64_t chunkExpected = 0;
    uint64_t chunkGot = 0;
    uint64_t preallocatedSize = 0;
    // How long the last failed SD write in this slice took. A multi-second
    // write is a full-FAT search; a sub-second one is a stuck transfer.
    uint32_t lastWriteMs = 0;
    static constexpr size_t kWriteBufferSize = 4096;
    std::unique_ptr<uint8_t[]> writeBuffer;
    size_t writeBufferPos = 0;
  } upload;

  void handleRoot() const;
  void handleStatus() const;
  void handleTimezone();
  void handleFileList() const;
  void handleFonts() const;
  void handleFontSelect();
  void handleFontDelete();
  void handleDownload() const;
  void handleMkdir() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;
  void handleNotFound() const;

  // Raw (non-multipart) upload: registered via a custom RequestHandler so
  // WebServer bulk-reads the body straight into our buffer instead of
  // parsing it byte-by-byte through multipart boundary matching. Path and
  // filename travel as request headers (X-File-Path/X-File-Name) since the
  // query string isn't parsed for raw-body requests. Slice position travels
  // as X-Upload-Offset and X-Upload-Total. See RawUploadHandler.
  //
  // Non-owning: WebServer::addHandler() takes ownership and deletes every
  // registered handler in ~WebServer(), so this must NOT also be deleted
  // here (that previously caused a double-free/heap corruption on stop()).
  class RawUploadHandler;
  RawUploadHandler* uploadHandler = nullptr;
  void handleUploadStart();
  void handleUploadChunk(const uint8_t* data, size_t len);
  void handleUploadEnd(size_t totalBytes);
  void handleUploadAbort();
  void handleUploadCancel();
  void sendUploadResponse() const;
  void writeUploadBytes(const uint8_t* data, size_t len);
  void flushWriteBuffer();
  // Position `upload.file` so this request's body is written at `offset`.
  // False leaves statusCode set and the previous prefix untouched.
  bool openPartial(const std::string& dest, uint64_t offset, uint64_t total, uint64_t chunkLen);
  // Shrink the open partial to `offset` and leave the handle there.
  bool rewindPartial(uint64_t offset);
  // Sync, close, and read the partial back. An EPUB must contain its end
  // marker. False deletes the partial and sets 409 so the page starts over.
  bool commitPartial();
  // Drop this request's bytes. The prefix before chunkOffset stays on the card.
  bool rollbackChunk();
  // Close the in-flight file. If `removePartial`, delete the .partial file.
  void resetUpload(bool removePartial);
};
