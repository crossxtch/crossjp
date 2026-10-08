#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

namespace ts {

class ZipArchive {
 public:
  // Central-directory records, directories included. A 7-volume EPUB can
  // list several hundred files. Inline 96-byte names made that table larger
  // than the free DRAM block left after the framebuffer is allocated.
  static constexpr uint16_t kMaxEntries = 4096;

  struct Entry {
    // Points into the same allocation as the entry table.
    const char* name = nullptr;
    uint32_t localOff = 0;
    uint32_t compSize = 0;
    uint32_t uncompSize = 0;
    uint16_t method = 0;
  };

  ZipArchive() = default;
  ~ZipArchive() { close(); }
  ZipArchive(const ZipArchive&) = delete;
  ZipArchive& operator=(const ZipArchive&) = delete;

  bool open(const char* path);
  void close();
  // Drop the SD handle and open it again. The central directory stays in RAM.
  // Ingest creates the atom file while this handle is released.
  void releaseFile();
  bool reopenFile();
  const Entry* find(const char* name) const;
  const Entry* findSuffix(const char* suffix) const;
  // 0xFFFF when `e` is not one of this archive's entries.
  uint16_t indexOf(const Entry* e) const;

  // Inflate into malloc'd buffer (caller free()). nullptr on failure.
  uint8_t* extract(const Entry& e, size_t* outLen);
  bool extractToFile(const Entry& e, const char* destPath);

  uint16_t count() const { return n; }
  const Entry& at(uint16_t i) const { return entries[i]; }
  const char* lastError() const { return error; }

 private:
  HalFile file;
  char openedPath[256]{};
  // One calloc: Entry[n] followed by the NUL-terminated names.
  Entry* entries = nullptr;
  uint16_t n = 0;
  const char* error = "closed";
  uint32_t fileSize = 0;

  bool parseCentral();
  bool dataStart(const Entry& e, uint32_t& off);
};

}  // namespace ts
