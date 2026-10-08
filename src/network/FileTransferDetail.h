#pragma once

#include <cstdint>
#include <string>

// Path, JSON, and SD helpers shared by FileTransferServer.cpp and
// FileTransferUpload.cpp. Not part of the server's public API.
namespace xfer {

void appendJsonEscaped(std::string& out, const char* s);
std::string joinPath(const char* dir, const char* name);
std::string dirnameOf(const char* path);
const char* basenameOf(const char* path);
bool isProtectedPath(const char* path);
bool isPartialName(const char* name);
bool parseU64(const char* s, uint64_t& out);
bool tailHasEocd(const uint8_t* tail, uint32_t n);
bool endsWithIgnoreCase(const char* s, size_t n, const char* ext);
bool resolveUploadDest(const char* dir, const char* name, std::string& dest);
void pumpNetwork();
bool deleteDirContents(const char* path);

// WebServer::handleClient() sets a 5s socket timeout before parsing. A Wi-Fi
// hiccup makes client.readBytes() return 0, the raw parser aborts that slice,
// and the browser sends the same slice again. readBytes() also restarts this
// wait whenever any byte arrives, so a dribble stretches one slice toward a
// minute and the radio sits in that read while the retry is queued. Keep the
// quiet-gap short: the page waits slightly longer than this before resending.
constexpr uint32_t kUploadSocketTimeoutMs = 5000;
constexpr uint64_t kMaxPreallocateBytes = 2ull * 1024 * 1024;
constexpr size_t kMaxListEntries = 256;

}  // namespace xfer
