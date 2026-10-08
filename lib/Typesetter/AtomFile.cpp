#include "AtomFile.h"

#include <Logging.h>

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
  healthy = true;
  noSpace = false;
  if (!Storage.openFileForWrite("ATM", path, file)) {
    // exFAT refuses O_TRUNC when the bitmap does not match the directory
    // entry, and a shared-SPI read can leave the card mid-command. One
    // recover plus delete, then the caller can switch cache names.
    uint8_t sdErr = 0;
    Storage.recoverCard(&sdErr);
    const bool removed = path && path[0] && Storage.remove(path);
    LOG_ERR("ATM", "retry %s sd=0x%02X removed=%d", path ? path : "", sdErr, removed ? 1 : 0);
    if (!Storage.openFileForWrite("ATM", path, file)) {
      uint64_t freeB = 0;
      if (Storage.freeBytes(&freeB)) {
        LOG_ERR("ATM", "blocked %s free=%lu KB", path ? path : "", static_cast<unsigned long>(freeB / 1024));
      }
      return false;
    }
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

bool AtomWriter::commit(const uint8_t* buf, const uint8_t n) {
  if (n == 0) {
    return true;
  }
  if (file.write(buf, n) == n) {
    pos += n;
    return true;
  }
  // The card is often still inside the write that just timed out. Stop it before
  // seeking back, or the retry fails the same way. freeBytes walks the FAT, so
  // it runs only when the retry also fails.
  uint8_t sdErr = 0;
  Storage.recoverCard(&sdErr);
  file.clearWriteError();
  LOG_ERR("ATM", "short %u bytes at %lu sd=0x%02X", n, static_cast<unsigned long>(pos), sdErr);
  bool back = false;
  for (int attempt = 0; attempt < 2 && !back; ++attempt) {
    if (attempt > 0) {
      Storage.recoverCard(nullptr);
      file.clearWriteError();
    }
    back = file.seekSet(pos);
  }
  if (back && file.write(buf, n) == n) {
    LOG_INF("ATM", "retry ok at %lu", static_cast<unsigned long>(pos));
    pos += n;
    return true;
  }
  uint8_t again = 0;
  Storage.recoverCard(&again);
  file.clearWriteError();
  uint64_t freeB = 0;
  const bool knowFree = Storage.freeBytes(&freeB);
  noSpace = knowFree && freeB == 0;
  if (knowFree) {
    LOG_ERR("ATM", "retry failed at %lu sd=0x%02X free=%lu KB", static_cast<unsigned long>(pos), again,
            static_cast<unsigned long>(freeB / 1024));
  } else {
    LOG_ERR("ATM", "retry failed at %lu sd=0x%02X", static_cast<unsigned long>(pos), again);
  }
  healthy = false;
  return false;
}

bool AtomWriter::write(const Atom& a) {
  if (!healthy) {
    return false;
  }
  // Group is the widest: kind + base count + 4 bases + ruby count + 8 readings + emphasis.
  uint8_t buf[64];
  uint8_t n = 0;
  auto u8 = [&](const uint8_t v) { buf[n++] = v; };
  auto u32 = [&](const uint32_t v) {
    buf[n++] = static_cast<uint8_t>(v);
    buf[n++] = static_cast<uint8_t>(v >> 8);
    buf[n++] = static_cast<uint8_t>(v >> 16);
    buf[n++] = static_cast<uint8_t>(v >> 24);
  };
  u8(static_cast<uint8_t>(a.kind));
  switch (a.kind) {
    case AtomKind::Ch: {
      const uint8_t nRuby = a.rubyCount > 8 ? 8 : a.rubyCount;
      u32(a.cp);
      u8(nRuby);
      for (uint8_t i = 0; i < nRuby; ++i) {
        u32(a.ruby[i]);
      }
      u8(a.emphasis);
      break;
    }
    case AtomKind::Group: {
      const uint8_t nBase = a.tcyCount > 4 ? 4 : a.tcyCount;
      const uint8_t nRuby = a.rubyCount > 8 ? 8 : a.rubyCount;
      u8(nBase);
      for (uint8_t i = 0; i < nBase; ++i) {
        u32(a.tcy[i]);
      }
      u8(nRuby);
      for (uint8_t i = 0; i < nRuby; ++i) {
        u32(a.ruby[i]);
      }
      u8(a.emphasis);
      break;
    }
    case AtomKind::Mode:
      u8(static_cast<uint8_t>(a.cp));
      break;
    case AtomKind::Tcy: {
      const uint8_t count = a.tcyCount > 4 ? 4 : a.tcyCount;
      u8(count);
      for (uint8_t i = 0; i < count; ++i) {
        u32(i == 0 ? a.cp : a.tcy[i]);
      }
      u8(a.emphasis);
      break;
    }
    case AtomKind::ColumnBreak:
      u8(a.startEm);
      break;
    case AtomKind::Picture: {
      const uint16_t id = static_cast<uint16_t>(a.cp);
      u8(static_cast<uint8_t>(id));
      u8(static_cast<uint8_t>(id >> 8));
      break;
    }
    default:
      break;
  }
  return commit(buf, n);
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
    case AtomKind::Picture: {
      uint8_t b[2];
      if (file.read(b, 2) != 2) {
        return false;
      }
      pos += 2;
      a.cp = static_cast<uint32_t>(b[0] | (b[1] << 8));
      break;
    }
    default:
      break;
  }
  return true;
}

}  // namespace ts
