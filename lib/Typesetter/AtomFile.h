#pragma once

#include "Layout.h"

#include <HalStorage.h>

#include <cstdint>

namespace ts {

// IRA5. A file without this header is an older ingest and must be rebuilt.
constexpr uint32_t kAtomMagic = 0x35415249u;

class AtomWriter {
 public:
  bool open(const char* path);
  void close();
  bool write(const Atom& a);
  uint32_t position() const { return pos; }

 private:
  HalFile file;
  uint32_t pos = 0;
};

class AtomReader {
 public:
  bool open(const char* path);
  void close();
  bool seek(uint32_t byteOff);
  bool next(Atom& a);
  uint32_t position() const { return pos; }

 private:
  HalFile file;
  uint32_t pos = 0;
};

}  // namespace ts
