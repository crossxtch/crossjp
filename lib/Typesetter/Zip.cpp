#include "Zip.h"

#include <Logging.h>
#include <esp_heap_caps.h>
#include <puff.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ts {
namespace {

uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

struct ZipIn {
  HalFile* file;
  uint32_t remaining;
};

int zipRefill(unsigned char* buf, unsigned long cap, void* user) {
  auto* in = static_cast<ZipIn*>(user);
  if (in->remaining == 0) {
    return 0;
  }
  const unsigned long n = cap < in->remaining ? cap : in->remaining;
  const int got = in->file->read(buf, n);
  if (got <= 0) {
    return -1;
  }
  in->remaining -= static_cast<uint32_t>(got);
  return got;
}

struct CentralRec {
  uint16_t method = 0;
  uint32_t comp = 0;
  uint32_t uncomp = 0;
  uint32_t local = 0;
  bool dir = false;
};

// One central-directory record. `name` is NUL-terminated and at most 95 bytes,
// matching the old inline field. False means the record is cut off.
bool readCentral(HalFile& file, CentralRec& rec, char* name, const uint16_t nameCap) {
  uint8_t hdr[46];
  if (file.read(hdr, 46) != 46 || u32(hdr) != 0x02014b50) {
    return false;
  }
  rec.method = u16(hdr + 10);
  rec.comp = u32(hdr + 20);
  rec.uncomp = u32(hdr + 24);
  rec.local = u32(hdr + 42);
  const uint16_t nameLen = u16(hdr + 28);
  const uint16_t extraLen = u16(hdr + 30);
  const uint16_t commentLen = u16(hdr + 32);
  const uint16_t nameLimit = nameCap > 0 ? static_cast<uint16_t>(nameCap - 1) : 0;
  const uint16_t nameGot = nameLen < nameLimit ? nameLen : nameLimit;
  if (nameGot > 0 && file.read(name, nameGot) != nameGot) {
    return false;
  }
  if (nameCap > 0) {
    name[nameGot] = 0;
  }
  const uint32_t skip = static_cast<uint32_t>(nameLen > nameGot ? nameLen - nameGot : 0) + extraLen + commentLen;
  if (skip > 0 && !file.seekCur(static_cast<int64_t>(skip))) {
    return false;
  }
  rec.dir = nameGot == 0 || name[nameGot - 1] == '/';
  return true;
}

}  // namespace

bool ZipArchive::open(const char* path) {
  close();
  if (!path || path[0] == '\0') {
    error = "zip missing";
    return false;
  }
  snprintf(openedPath, sizeof(openedPath), "%s", path);
  if (!Storage.openFileForRead("ZIP", openedPath, file)) {
    error = "zip missing";
    return false;
  }
  fileSize = static_cast<uint32_t>(file.fileSize());
  if (fileSize < 22) {
    error = "zip tiny";
    return false;
  }
  if (parseCentral()) {
    error = "";
    return true;
  }
  // The progress paint shares SPI with the card. One retry after the bus is idle.
  if (strcmp(error, "zip tail") != 0) {
    close();
    return false;
  }
  const uint32_t sz = fileSize;
  close();
  snprintf(openedPath, sizeof(openedPath), "%s", path);
  LOG_ERR("ZIP", "tail unreadable size=%lu", static_cast<unsigned long>(sz));
  Storage.recoverCard(nullptr);
  if (!Storage.openFileForRead("ZIP", openedPath, file)) {
    error = "zip tail";
    return false;
  }
  fileSize = static_cast<uint32_t>(file.fileSize());
  if (fileSize < 22) {
    close();
    error = "zip tiny";
    return false;
  }
  if (!parseCentral()) {
    close();
    return false;
  }
  LOG_INF("ZIP", "tail read recovered size=%lu", static_cast<unsigned long>(fileSize));
  error = "";
  return true;
}

void ZipArchive::releaseFile() {
  if (file.isOpen()) {
    file.close();
  }
}

bool ZipArchive::reopenFile() {
  if (file.isOpen()) {
    return true;
  }
  if (openedPath[0] == '\0') {
    error = "zip missing";
    return false;
  }
  if (!Storage.openFileForRead("ZIP", openedPath, file)) {
    error = "zip missing";
    return false;
  }
  return true;
}

void ZipArchive::close() {
  releaseFile();
  free(entries);
  entries = nullptr;
  n = 0;
  fileSize = 0;
  openedPath[0] = '\0';
}

const ZipArchive::Entry* ZipArchive::find(const char* name) const {
  if (!name || !entries) {
    return nullptr;
  }
  for (uint16_t i = 0; i < n; ++i) {
    if (strcmp(entries[i].name, name) == 0) {
      return &entries[i];
    }
    // OPF hrefs are relative; also match suffix.
    const size_t nl = strlen(name);
    const size_t el = strlen(entries[i].name);
    if (el >= nl && strcmp(entries[i].name + (el - nl), name) == 0) {
      if (el == nl || entries[i].name[el - nl - 1] == '/') {
        return &entries[i];
      }
    }
  }
  return nullptr;
}

const ZipArchive::Entry* ZipArchive::findSuffix(const char* suffix) const {
  if (!suffix || !entries) {
    return nullptr;
  }
  const size_t sl = strlen(suffix);
  for (uint16_t i = 0; i < n; ++i) {
    const size_t el = strlen(entries[i].name);
    if (el >= sl && strcmp(entries[i].name + (el - sl), suffix) == 0) {
      return &entries[i];
    }
  }
  return nullptr;
}

uint16_t ZipArchive::indexOf(const Entry* e) const {
  if (!entries || !e || e < entries || e >= entries + n) {
    return 0xFFFFu;
  }
  return static_cast<uint16_t>(e - entries);
}

bool ZipArchive::parseCentral() {
  // EOCD is 22 bytes at EOF (EPUB comments are empty). Search a 1 KB tail.
  uint8_t tail[1024];
  const uint32_t tailN = fileSize < sizeof(tail) ? fileSize : sizeof(tail);
  const uint32_t tailOff = fileSize - tailN;
  if (!file.seekSet(tailOff) || file.read(tail, tailN) != static_cast<int>(tailN)) {
    error = "zip tail";
    return false;
  }
  int eocd = -1;
  for (int i = static_cast<int>(tailN) - 22; i >= 0; --i) {
    if (u32(tail + i) == 0x06054b50) {
      eocd = i;
      break;
    }
  }
  if (eocd < 0) {
    error = "no eocd";
    return false;
  }
  const uint16_t entriesN = u16(tail + eocd + 10);
  const uint32_t cdOff = u32(tail + eocd + 16);
  if (entriesN == 0 || entriesN > kMaxEntries) {
    LOG_ERR("ZIP", "cd entries %u", entriesN);
    error = "zip cd";
    return false;
  }
  if (!file.seekSet(cdOff)) {
    error = "cd seek";
    return false;
  }

  // Count files first so the table and the names are one exact allocation.
  // A second walk of a few dozen KB is cheaper than a 96-byte name on every row.
  uint16_t files = 0;
  uint32_t nameBytes = 0;
  for (uint16_t k = 0; k < entriesN; ++k) {
    CentralRec rec;
    char name[96];
    if (!readCentral(file, rec, name, sizeof(name))) {
      break;
    }
    if (rec.dir) {
      continue;
    }
    ++files;
    nameBytes += static_cast<uint32_t>(strlen(name) + 1);
  }
  if (files == 0) {
    error = "empty zip";
    return false;
  }

  free(entries);
  entries = nullptr;
  const size_t entryBytes = static_cast<size_t>(files) * sizeof(Entry);
  void* block = calloc(1, entryBytes + nameBytes);
  if (!block) {
    LOG_ERR("ZIP", "ent oom files=%u names=%lu", files, static_cast<unsigned long>(nameBytes));
    error = "ent oom";
    return false;
  }
  entries = static_cast<Entry*>(block);
  char* cursor = static_cast<char*>(block) + entryBytes;
  char* const nameEnd = cursor + nameBytes;

  if (!file.seekSet(cdOff)) {
    error = "cd seek";
    return false;
  }
  n = 0;
  for (uint16_t k = 0; k < entriesN && n < files; ++k) {
    CentralRec rec;
    char name[96];
    if (!readCentral(file, rec, name, sizeof(name))) {
      break;
    }
    if (rec.dir) {
      continue;
    }
    const size_t len = strlen(name);
    if (cursor + len + 1 > nameEnd) {
      break;
    }
    memcpy(cursor, name, len + 1);
    Entry& e = entries[n];
    e.name = cursor;
    e.localOff = rec.local;
    e.compSize = rec.comp;
    e.uncompSize = rec.uncomp;
    e.method = rec.method;
    cursor += len + 1;
    ++n;
  }
  if (n == 0) {
    error = "empty zip";
    return false;
  }
  LOG_INF("ZIP", "%u entries (%lu bytes)", n, static_cast<unsigned long>(entryBytes + nameBytes));
  return true;
}

bool ZipArchive::dataStart(const Entry& e, uint32_t& off) {
  uint8_t loc[30];
  if (!file.seekSet(e.localOff) || file.read(loc, 30) != 30) {
    return false;
  }
  if (u32(loc) != 0x04034b50) {
    return false;
  }
  const uint16_t nameLen = u16(loc + 26);
  const uint16_t extraLen = u16(loc + 28);
  off = e.localOff + 30 + nameLen + extraLen;
  return true;
}

uint8_t* ZipArchive::extract(const Entry& e, size_t* outLen) {
  if (outLen) {
    *outLen = 0;
  }
  uint32_t need = e.uncompSize;
  if (need == 0 && e.method == 8 && e.compSize > 0) {
    need = e.compSize * 8;
    if (need < 4096) {
      need = 4096;
    }
  }
  if (need == 0) {
    auto* z = static_cast<uint8_t*>(malloc(1));
    if (z) {
      z[0] = 0;
    }
    return z;
  }
  uint8_t* dest = static_cast<uint8_t*>(malloc(need + 1));
  if (!dest) {
    LOG_ERR("ZIP", "member too big %s uncomp=%lu comp=%lu block=%u", e.name ? e.name : "?",
            static_cast<unsigned long>(e.uncompSize), static_cast<unsigned long>(e.compSize),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    error = "member too big";
    return nullptr;
  }
  uint32_t dataOff = 0;
  if (!dataStart(e, dataOff)) {
    free(dest);
    error = "local hdr";
    return nullptr;
  }
  LOG_DBG("ZIP", "extract %s method=%u comp=%u uncomp=%u", e.name, e.method, static_cast<unsigned>(e.compSize),
          static_cast<unsigned>(need));
  if (e.method == 0) {
    if (!file.seekSet(dataOff) || file.read(dest, need) != static_cast<int>(need)) {
      free(dest);
      error = "store read";
      return nullptr;
    }
    dest[need] = 0;
    if (outLen) {
      *outLen = need;
    }
    return dest;
  }
  if (e.method != 8) {
    free(dest);
    error = "zip method";
    return nullptr;
  }
  if (!file.seekSet(dataOff)) {
    free(dest);
    return nullptr;
  }
  unsigned long destlen = need;
  unsigned char inbuf[512];
  ZipIn zin{&file, e.compSize};
  const int st = puff_stream(dest, &destlen, zipRefill, &zin, inbuf, sizeof(inbuf));
  if (st != 0) {
    LOG_ERR("ZIP", "puff %s st=%d", e.name, st);
    free(dest);
    error = "inflate";
    return nullptr;
  }
  dest[destlen <= need ? destlen : need] = 0;
  if (outLen) {
    *outLen = destlen <= need ? destlen : need;
  }
  return dest;
}

bool ZipArchive::extractToFile(const Entry& e, const char* destPath) {
  uint32_t dataOff = 0;
  if (!dataStart(e, dataOff) || !file.seekSet(dataOff)) {
    error = "local hdr";
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("ZIP", destPath, out)) {
    error = "work file";
    return false;
  }
  if (e.method == 0) {
    uint8_t chunk[512];
    uint32_t left = e.uncompSize ? e.uncompSize : e.compSize;
    while (left > 0) {
      const uint32_t n = left > sizeof(chunk) ? sizeof(chunk) : left;
      const int got = file.read(chunk, n);
      if (got <= 0 || out.write(chunk, static_cast<size_t>(got)) != static_cast<size_t>(got)) {
        error = "store write";
        return false;
      }
      left -= static_cast<uint32_t>(got);
    }
    if (!out.sync()) {
      error = "work sync";
      return false;
    }
    out.close();
    error = "";
    LOG_INF("ZIP", "stored %s %u bytes", e.name, static_cast<unsigned>(e.uncompSize));
    return true;
  }
  if (e.method != 8) {
    error = "zip method";
    return false;
  }
  unsigned char* window = static_cast<unsigned char*>(malloc(32768));
  if (!window) {
    LOG_ERR("ZIP", "window oom %s block=%u", e.name ? e.name : "?",
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    error = "window oom";
    return false;
  }
  unsigned char inbuf[512];
  ZipIn zin{&file, e.compSize};
  auto flush = [](const unsigned char* p, unsigned long n, void* user) -> int {
    auto* f = static_cast<HalFile*>(user);
    return f->write(p, n) == n ? 0 : 1;
  };
  unsigned long produced = 0;
  const unsigned long t0 = millis();
  const int st = puff_stream_out(zipRefill, &zin, inbuf, sizeof(inbuf), window, 32768, flush, &out, &produced);
  free(window);
  if (st != 0) {
    LOG_ERR("ZIP", "puff-out %s st=%d", e.name, st);
    error = "inflate";
    return false;
  }
  if (!out.sync()) {
    error = "work sync";
    out.close();
    return false;
  }
  out.close();
  error = "";
  LOG_INF("ZIP", "streamed %s %u bytes %lums", e.name, static_cast<unsigned>(produced), millis() - t0);
  return true;
}

}  // namespace ts
