#pragma once

#include "Layout.h"

#include <HalStorage.h>

#include <cstdint>

namespace ts {

// IRA6. Picture atoms carry a page id. A file without this header is rebuilt.
constexpr uint32_t kAtomMagic = 0x36415249u;

class AtomWriter {
 public:
  bool open(const char* path);
  void close();
  bool write(const Atom& a);
  // False after a short write that did not recover. Later writes do not touch the card.
  bool ok() const { return healthy; }
  bool diskFull() const { return noSpace; }
  uint32_t position() const { return pos; }

 private:
  // One SdFat write for the packed atom. A short write is retried once: FatFile::write
  // returns 0 after the card has already moved, and this bus shares SPI with the panel.
  bool commit(const uint8_t* buf, uint8_t n);

  HalFile file;
  uint32_t pos = 0;
  bool healthy = true;
  bool noSpace = false;
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
