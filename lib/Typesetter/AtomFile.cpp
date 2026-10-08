#include "AtomFile.h"

#include <cstring>

namespace ts {
namespace {

bool writeU8(HalFile& f, const uint8_t v) { return f.write(&v, 1) == 1; }
bool writeU32(HalFile& f, const uint32_t v) {
  const uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                        static_cast<uint8_t>(v >> 24)};
  return f.write(b, 4) == 4;
}
bool readU8(HalFile& f, uint8_t& v) { return f.read(&v, 1) == 1; }
bool readU32(HalFile& f, uint32_t& v) {
  uint8_t b[4];
  if (f.read(b, 4) != 4) {
    return false;
  }
  v = static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
  return true;
}

}  // namespace

bool AtomWriter::open(const char* path) {
  pos = 0;
  if (!Storage.openFileForWrite("ATM", path, file)) {
    return false;
  }
  if (!writeU32(file, kAtomMagic)) {
    close();
    return false;
  }
  pos = 4;
  return true;
}

void AtomWriter::close() {
  if (file.isOpen()) {
    file.close();
  }
}

bool AtomWriter::write(const Atom& a) {
  if (!writeU8(file, static_cast<uint8_t>(a.kind))) {
    return false;
  }
  pos += 1;
  switch (a.kind) {
    case AtomKind::Ch: {
      const uint8_t nRuby = a.rubyCount > 8 ? 8 : a.rubyCount;
      if (!writeU32(file, a.cp) || !writeU8(file, nRuby)) {
        return false;
      }
      pos += 5;
      for (uint8_t i = 0; i < nRuby; ++i) {
        if (!writeU32(file, a.ruby[i])) {
          return false;
        }
        pos += 4;
      }
      if (!writeU8(file, a.emphasis)) {
        return false;
      }
      pos += 1;
      break;
    }
    case AtomKind::Group: {
      const uint8_t nBase = a.tcyCount > 4 ? 4 : a.tcyCount;
      const uint8_t nRuby = a.rubyCount > 8 ? 8 : a.rubyCount;
      if (!writeU8(file, nBase)) {
        return false;
      }
      pos += 1;
      for (uint8_t i = 0; i < nBase; ++i) {
        if (!writeU32(file, a.tcy[i])) {
          return false;
        }
        pos += 4;
      }
      if (!writeU8(file, nRuby)) {
        return false;
      }
      pos += 1;
      for (uint8_t i = 0; i < nRuby; ++i) {
        if (!writeU32(file, a.ruby[i])) {
          return false;
        }
        pos += 4;
      }
      if (!writeU8(file, a.emphasis)) {
        return false;
      }
      pos += 1;
      break;
    }
    case AtomKind::Mode:
      if (!writeU8(file, static_cast<uint8_t>(a.cp))) {
        return false;
      }
      pos += 1;
      break;
    case AtomKind::Tcy: {
      const uint8_t n = a.tcyCount > 4 ? 4 : a.tcyCount;
      if (!writeU8(file, n)) {
        return false;
      }
      pos += 1;
      for (uint8_t i = 0; i < n; ++i) {
        if (!writeU32(file, i == 0 ? a.cp : a.tcy[i])) {
          return false;
        }
        pos += 4;
      }
      if (!writeU8(file, a.emphasis)) {
        return false;
      }
      pos += 1;
      break;
    }
    case AtomKind::ColumnBreak:
      if (!writeU8(file, a.startEm)) {
        return false;
      }
      pos += 1;
      break;
    default:
      break;
  }
  return true;
}

bool AtomReader::open(const char* path) {
  pos = 0;
  if (!Storage.openFileForRead("ATM", path, file)) {
    return false;
  }
  uint32_t magic = 0;
  if (!readU32(file, magic) || magic != kAtomMagic) {
    close();
    return false;
  }
  pos = 4;
  return true;
}

void AtomReader::close() {
  if (file.isOpen()) {
    file.close();
  }
}

bool AtomReader::seek(const uint32_t byteOff) {
  pos = byteOff;
  return file.seekSet(byteOff);
}

bool AtomReader::next(Atom& a) {
  a = Atom{};
  uint8_t kind = 0;
  if (!readU8(file, kind)) {
    return false;
  }
  pos += 1;
  a.kind = static_cast<AtomKind>(kind);
  switch (a.kind) {
    case AtomKind::Ch: {
      if (!readU32(file, a.cp) || !readU8(file, a.rubyCount)) {
        return false;
      }
      pos += 5;
      const uint8_t n = a.rubyCount;
      const uint8_t keep = n > 8 ? 8 : n;
      for (uint8_t i = 0; i < n; ++i) {
        uint32_t cp = 0;
        if (!readU32(file, cp)) {
          return false;
        }
        pos += 4;
        if (i < keep) {
          a.ruby[i] = cp;
        }
      }
      a.rubyCount = keep;
      if (!readU8(file, a.emphasis)) {
        return false;
      }
      pos += 1;
      break;
    }
    case AtomKind::Group: {
      if (!readU8(file, a.tcyCount)) {
        return false;
      }
      pos += 1;
      const uint8_t nBase = a.tcyCount;
      const uint8_t keepBase = nBase > 4 ? 4 : nBase;
      for (uint8_t i = 0; i < nBase; ++i) {
        uint32_t cp = 0;
        if (!readU32(file, cp)) {
          return false;
        }
        pos += 4;
        if (i < keepBase) {
          a.tcy[i] = cp;
        }
      }
      a.tcyCount = keepBase;
      a.cp = keepBase > 0 ? a.tcy[0] : 0;
      if (!readU8(file, a.rubyCount)) {
        return false;
      }
      pos += 1;
      const uint8_t nRuby = a.rubyCount;
      const uint8_t keepRuby = nRuby > 8 ? 8 : nRuby;
      for (uint8_t i = 0; i < nRuby; ++i) {
        uint32_t cp = 0;
        if (!readU32(file, cp)) {
          return false;
        }
        pos += 4;
        if (i < keepRuby) {
          a.ruby[i] = cp;
        }
      }
      a.rubyCount = keepRuby;
      if (!readU8(file, a.emphasis)) {
        return false;
      }
      pos += 1;
      break;
    }
    case AtomKind::Mode:
      if (!readU8(file, a.startEm)) {
        return false;
      }
      pos += 1;
      a.cp = a.startEm;
      a.startEm = 0;
      break;
    case AtomKind::Tcy: {
      uint8_t n = 0;
      if (!readU8(file, n)) {
        return false;
      }
      pos += 1;
      const uint8_t keep = n > 4 ? 4 : n;
      for (uint8_t i = 0; i < n; ++i) {
        uint32_t cp = 0;
        if (!readU32(file, cp)) {
          return false;
        }
        pos += 4;
        if (i < keep) {
          a.tcy[i] = cp;
          if (i == 0) {
            a.cp = cp;
          }
        }
      }
      a.tcyCount = keep;
      if (!readU8(file, a.emphasis)) {
        return false;
      }
      pos += 1;
      break;
    }
    case AtomKind::ColumnBreak:
      if (!readU8(file, a.startEm)) {
        return false;
      }
      pos += 1;
      break;
    default:
      break;
  }
  return true;
}

}  // namespace ts
