#include "TypesetBook.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

bool endsWithI(const char* name, const char* ext) {
  const size_t n = strlen(name);
  const size_t e = strlen(ext);
  if (n < e) {
    return false;
  }
  for (size_t i = 0; i < e; ++i) {
    char a = name[n - e + i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (a != ext[i]) {
      return false;
    }
  }
  return true;
}

uint32_t fileSizeOf(const char* path) {
  HalFile f;
  if (!path || !Storage.openFileForRead("TS", path, f)) {
    return 0;
  }
  return static_cast<uint32_t>(f.fileSize());
}

bool copyBytes(HalFile& in, HalFile& out, uint32_t nbytes) {
  uint8_t buf[504];
  while (nbytes > 0) {
    const uint32_t chunk = nbytes > sizeof(buf) ? static_cast<uint32_t>(sizeof(buf)) : nbytes;
    const int got = in.read(buf, chunk);
    if (got != static_cast<int>(chunk) || out.write(buf, chunk) != chunk) {
      return false;
    }
    nbytes -= chunk;
  }
  return true;
}

}  // namespace

uint16_t TypesetBook::pageCount() const {
  if (nPages == 0) {
    return 0;
  }
  const uint32_t shown = static_cast<uint32_t>(frontPages) + nPages;
  if (shown > 65535u) {
    return 65535;
  }
  if (buildFinished()) {
    return static_cast<uint16_t>(shown);
  }
  uint32_t projected = atomBytes > 0 ? atomBytes : sourceSize;
  if (!ingestDone && nextSpine > 0 && spineCount > nextSpine && projected > 0) {
    const uint32_t scaled =
        static_cast<uint32_t>((static_cast<uint64_t>(projected) * spineCount) / nextSpine);
    if (scaled > projected) {
      projected = scaled;
    }
  }
  const uint32_t span = indexedAtom > originAtom ? indexedAtom - originAtom : 0;
  uint32_t est = shown;
  if (span > 0 && projected > indexedAtom) {
    est = shown + static_cast<uint32_t>((static_cast<uint64_t>(nPages) * (projected - indexedAtom)) / span);
  }
  if (est < shown) {
    est = shown;
  }
  if (est > 65535u) {
    est = 65535u;
  }
  return static_cast<uint16_t>(est);
}

uint32_t TypesetBook::localOf(const uint32_t globalPage) const {
  if (globalPage < frontPages) {
    return 0xFFFFFFFFu;
  }
  const uint32_t local = globalPage - frontPages;
  if (local >= nPages) {
    return 0xFFFFFFFFu;
  }
  return local;
}

uint32_t TypesetBook::atomForPage(const uint32_t globalPage) const {
  const uint32_t local = localOf(globalPage);
  if (local == 0xFFFFFFFFu) {
    return globalPage < frontPages ? originAtom : indexedAtom;
  }
  uint32_t off = 0;
  uint8_t mode = 0;
  uint16_t pic = 0;
  if (!readPage(local, off, mode, pic)) {
    return originAtom;
  }
  return off;
}

uint32_t TypesetBook::globalForAtom(const uint32_t atom) const {
  if (nPages == 0 || atom < originAtom) {
    return frontPages;
  }
  int lo = 0;
  int hi = static_cast<int>(nPages) - 1;
  int found = 0;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    uint32_t off = 0;
    uint8_t mode = 0;
    uint16_t pic = 0;
    if (!readPage(static_cast<uint32_t>(mid), off, mode, pic)) {
      break;
    }
    if (off <= atom) {
      found = mid;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  return static_cast<uint32_t>(frontPages) + static_cast<uint32_t>(found);
}

bool TypesetBook::pageCovers(const uint32_t atom) const {
  if (nPages == 0 || atom < originAtom) {
    return false;
  }
  int lo = 0;
  int hi = static_cast<int>(nPages) - 1;
  int found = -1;
  uint32_t foundOff = 0;
  while (lo <= hi) {
    const int mid = lo + (hi - lo) / 2;
    uint32_t off = 0;
    uint8_t mode = 0;
    uint16_t pic = 0;
    if (!readPage(static_cast<uint32_t>(mid), off, mode, pic)) {
      return false;
    }
    if (off <= atom) {
      found = mid;
      foundOff = off;
      lo = mid + 1;
    } else {
      hi = mid - 1;
    }
  }
  if (found < 0) {
    return false;
  }
  if (found + 1 < static_cast<int>(nPages)) {
    return true;
  }
  return foundOff <= atom && (coversEnd || indexedAtom > atom);
}

void TypesetBook::freezeFront() {
  if (frontFrozen) {
    return;
  }
  if (originAtom <= 4) {
    frontPages = 0;
    frontFrozen = true;
    return;
  }
  const uint32_t span = indexedAtom > originAtom ? indexedAtom - originAtom : 0;
  if (span == 0 || nPages == 0) {
    return;
  }
  uint32_t est = static_cast<uint32_t>((static_cast<uint64_t>(nPages) * (originAtom - 4)) / span);
  if (est < 1) {
    est = 1;
  }
  if (est > 65534u) {
    est = 65534u;
  }
  frontPages = static_cast<uint16_t>(est);
  frontFrozen = true;
}

void TypesetBook::cursorPath(char* out, const size_t outSize) const {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(filepath); *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  snprintf(out, outSize, "/.crossjp/s_%08lx.bin", static_cast<unsigned long>(h));
}

void TypesetBook::dropPartialCache() {
  char p[64];
  cursorPath(p, sizeof(p));
  Storage.remove(p);
  indexPath(p, sizeof(p));
  Storage.remove(p);
  Storage.remove(kWorkIndex);
  chapterMarks.clear();
  nextSpine = 0;
  spineCount = 0;
  atomBytes = 0;
  cursorOk = false;
  ingestDone = false;
  coversEnd = false;
  pendingPage = false;
  frontFrozen = false;
  frontPages = 0;
  originAtom = 4;
  indexedAtom = 4;
  originSpine = 0;
  nPages = 0;
  indexBufN = 0;
}

bool TypesetBook::loadCursor() {
  cursorOk = false;
  char p[64];
  cursorPath(p, sizeof(p));
  HalFile f;
  if (!Storage.openFileForRead("TS", p, f)) {
    return false;
  }
  uint32_t magic = 0;
  uint32_t src = 0;
  uint32_t bytes = 0;
  uint16_t nSp = 0;
  uint16_t next = 0;
  uint16_t mode = 0;
  uint16_t n = 0;
  uint8_t titleLen = 0;
  if (f.read(&magic, 4) != 4 || magic != kCursorMagic || f.read(&src, 4) != 4 || f.read(&nSp, 2) != 2 ||
      f.read(&next, 2) != 2 || f.read(&bytes, 4) != 4 || f.read(&mode, 2) != 2 || f.read(&titleLen, 1) != 1 ||
      titleLen >= sizeof(bookTitle)) {
    return false;
  }
  char title[128]{};
  if (titleLen > 0 && f.read(title, titleLen) != static_cast<int>(titleLen)) {
    return false;
  }
  title[titleLen] = 0;
  if (f.read(&n, 2) != 2 || n > 512) {
    return false;
  }
  if (bookSrcSize != 0 && src != bookSrcSize) {
    LOG_INF("TS", "Cursor source mismatch");
    return false;
  }
  std::vector<ChapterMark> marks;
  marks.reserve(n);
  for (uint16_t i = 0; i < n; ++i) {
    ChapterMark m;
    uint8_t len = 0;
    if (f.read(&m.spine, 2) != 2 || f.read(&m.atomOff, 4) != 4 || f.read(&len, 1) != 1 || len >= sizeof(m.name)) {
      return false;
    }
    if (len > 0 && f.read(m.name, len) != static_cast<int>(len)) {
      return false;
    }
    m.name[len] = 0;
    marks.push_back(m);
  }
  if (title[0]) {
    snprintf(bookTitle, sizeof(bookTitle), "%s", title);
  }
  layoutOpt.mode = static_cast<ts::WritingMode>(mode);
  runMode = mode;
  spineCount = nSp;
  nextSpine = next;
  atomBytes = bytes;
  chapterMarks.swap(marks);
  cursorOk = true;
  LOG_INF("TS", "Cursor spines %u/%u atoms=%lu chapters=%u", static_cast<unsigned>(next), static_cast<unsigned>(nSp),
          static_cast<unsigned long>(bytes), static_cast<unsigned>(n));
  return true;
}

bool TypesetBook::saveCursor() const {
  char p[64];
  cursorPath(p, sizeof(p));
  HalFile f;
  if (!Storage.openFileForWrite("TS", p, f)) {
    return false;
  }
  const uint32_t magic = kCursorMagic;
  const uint32_t src = bookSrcSize;
  const uint16_t nSp = spineCount;
  const uint16_t next = nextSpine;
  const uint32_t bytes = atomBytes;
  const uint16_t mode = static_cast<uint16_t>(layoutOpt.mode);
  uint8_t titleLen = static_cast<uint8_t>(strlen(bookTitle));
  if (titleLen > 127) {
    titleLen = 127;
  }
  const uint16_t n = static_cast<uint16_t>(chapterMarks.size() > 512 ? 512 : chapterMarks.size());
  if (f.write(&magic, 4) != 4 || f.write(&src, 4) != 4 || f.write(&nSp, 2) != 2 || f.write(&next, 2) != 2 ||
      f.write(&bytes, 4) != 4 || f.write(&mode, 2) != 2 || f.write(&titleLen, 1) != 1) {
    return false;
  }
  if (titleLen > 0 && f.write(bookTitle, titleLen) != titleLen) {
    return false;
  }
  if (f.write(&n, 2) != 2) {
    return false;
  }
  for (uint16_t i = 0; i < n; ++i) {
    const size_t raw = strlen(chapterMarks[i].name);
    const uint8_t len = static_cast<uint8_t>(raw > 79 ? 79 : raw);
    if (f.write(&chapterMarks[i].spine, 2) != 2 || f.write(&chapterMarks[i].atomOff, 4) != 4 ||
        f.write(&len, 1) != 1) {
      return false;
    }
    if (len > 0 && f.write(chapterMarks[i].name, len) != len) {
      return false;
    }
  }
  return true;
}

bool TypesetBook::peekAtom(const uint32_t globalPage, uint32_t& atom) const {
  char p[64];
  indexPath(p, sizeof(p));
  HalFile f;
  if (!Storage.openFileForRead("TS", p, f)) {
    return false;
  }
  uint32_t magic = 0;
  if (f.read(&magic, 4) != 4) {
    return false;
  }
  uint32_t count = 0;
  uint16_t front = 0;
  uint32_t base = 0;
  if (magic == kIndexMagic) {
    base = kIndexPayload;
  } else if (magic == kIndexPartial) {
    base = kPartialPayload;
    if (!f.seekSet(42) || f.read(&front, 2) != 2) {
      return false;
    }
  } else {
    return false;
  }
  if (!f.seekSet(20) || f.read(&count, 4) != 4 || count == 0 || count > kMaxPages || globalPage < front) {
    return false;
  }
  const uint32_t local = globalPage - front;
  if (local >= count || !f.seekSet(base + local * 7)) {
    return false;
  }
  uint8_t b[4];
  if (f.read(b, 4) != 4) {
    return false;
  }
  atom = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
  return atom >= 4;
}

void TypesetBook::mergeToc(const ts::EpubBook& epub) {
  if (epub.tocCount() == 0) {
    return;
  }
  if (chapterMarks.capacity() < epub.tocCount()) {
    chapterMarks.reserve(epub.tocCount());
  }
  std::vector<uint8_t> used(chapterMarks.size(), 0);
  for (uint16_t i = 0; i < epub.tocCount(); ++i) {
    const auto& t = epub.toc(i);
    if (t.title[0] == 0) {
      continue;
    }
    int found = -1;
    if (t.spine != 0xFFFFu) {
      for (size_t k = 0; k < chapterMarks.size(); ++k) {
        if (!used[k] && chapterMarks[k].spine == t.spine) {
          found = static_cast<int>(k);
          used[k] = 1;
          break;
        }
      }
    }
    if (found < 0) {
      ChapterMark m;
      m.atomOff = t.atomOff;
      m.spine = t.spine;
      snprintf(m.name, sizeof(m.name), "%s", t.title);
      if (m.atomOff == 0xFFFFFFFFu) {
        for (const auto& old : chapterMarks) {
          if (old.atomOff != 0xFFFFFFFFu && strcmp(old.name, m.name) == 0) {
            m.atomOff = old.atomOff;
            break;
          }
        }
      }
      chapterMarks.push_back(m);
      continue;
    }
    ChapterMark& mark = chapterMarks[static_cast<size_t>(found)];
    if (t.atomOff != 0xFFFFFFFFu) {
      mark.atomOff = t.atomOff;
    }
    if (mark.name[0] == 0) {
      snprintf(mark.name, sizeof(mark.name), "%s", t.title);
    }
  }
}

bool TypesetBook::reopenAtoms() {
  atoms.close();
  if (!atoms.open(atomPath)) {
    atomsLive = false;
    error = "atom open";
    return false;
  }
  atomsLive = true;
  return true;
}

bool TypesetBook::reopenIndex() {
  if (nPages == 0) {
    char p[64];
    indexPath(p, sizeof(p));
    if (!Storage.exists(p)) {
      return true;
    }
  }
  if (!indexFile) {
    char p[64];
    indexPath(p, sizeof(p));
    indexFile = Storage.open(p, O_RDWR);
    if (!indexFile) {
      error = "index file";
      return false;
    }
  }
  const uint32_t at = static_cast<uint32_t>(indexBase) + static_cast<uint32_t>(nPages) * 7;
  if (!indexFile.seekSet(at)) {
    error = "index file";
    return false;
  }
  return true;
}

bool TypesetBook::ingestSlice(const uint16_t maxSpines, ts::EpubBook::ProgressFn progress, void* progressCtx) {
  if (ingestDone || !endsWithI(filepath, ".epub")) {
    return false;
  }
  font.releaseMaps();
  font.releaseFile();
  atoms.close();
  atomsLive = false;
  if (indexFile) {
    suspendIndex();
    indexFile.close();
  }

  ts::EpubBook epub;
  const uint16_t slice = maxSpines == 0 ? 1 : maxSpines;
  const bool append = atomBytes >= 4;
  const bool ok = epub.open(filepath, atomPath, progress, progressCtx, static_cast<uint16_t>(layoutOpt.width),
                            static_cast<uint16_t>(layoutOpt.height), picturePath, nextSpine, slice, append, atomBytes);
  if (ok) {
    if (epub.title()[0]) {
      snprintf(bookTitle, sizeof(bookTitle), "%s", epub.title());
    }
    layoutOpt.mode = epub.bookMode();
    nextSpine = epub.ingestedEnd();
    spineCount = epub.spineCount();
    atomBytes = fileSizeOf(atomPath);
    sourceSize = atomBytes;
    ingestDone = spineCount > 0 && nextSpine >= spineCount;
    if (atomBytes > indexedAtom) {
      coversEnd = false;
    }
    mergeToc(epub);
    saveCursor();
  }
  const char* epubError = epub.lastError();
  epub.close();
  if (!font.reopenFile() || !font.restoreMaps()) {
    error = "font maps";
    return false;
  }
  if (!ok) {
    error = epubError && epubError[0] ? epubError : "epub";
    reopenAtoms();
    reopenIndex();
    return false;
  }
  if (!reopenAtoms() || !reopenIndex()) {
    return false;
  }
  LOG_INF("TS", "Slice spine %u/%u atoms=%lu", static_cast<unsigned>(nextSpine), static_cast<unsigned>(spineCount),
          static_cast<unsigned long>(atomBytes));
  return true;
}

bool TypesetBook::writePartialHeader() {
  if (!indexFile.seekSet(0)) {
    error = "index write";
    return false;
  }
  const uint32_t magic = kIndexPartial;
  const uint32_t atomSz = sourceSize;
  const uint32_t srcSz = bookSrcSize;
  const uint16_t em = layoutOpt.em;
  const uint16_t w = static_cast<uint16_t>(layoutOpt.width);
  const uint16_t h = static_cast<uint16_t>(layoutOpt.height);
  const uint16_t modeWord = static_cast<uint16_t>(layoutOpt.mode);
  const uint32_t count = nPages;
  const uint32_t flags = (ingestDone ? kFlagIngest : 0u) | (coversEnd ? kFlagCovers : 0u) |
                         (pendingPage ? kFlagPending : 0u) | (frontFrozen ? kFlagFrozen : 0u);
  const uint32_t pad = runMode;
  if (indexFile.write(&magic, 4) != 4 || indexFile.write(&atomSz, 4) != 4 || indexFile.write(&srcSz, 4) != 4 ||
      indexFile.write(&em, 2) != 2 || indexFile.write(&w, 2) != 2 || indexFile.write(&h, 2) != 2 ||
      indexFile.write(&modeWord, 2) != 2 || indexFile.write(&count, 4) != 4 || indexFile.write(&originAtom, 4) != 4 ||
      indexFile.write(&indexedAtom, 4) != 4 || indexFile.write(&flags, 4) != 4 ||
      indexFile.write(&nextSpine, 2) != 2 || indexFile.write(&spineCount, 2) != 2 ||
      indexFile.write(&originSpine, 2) != 2 || indexFile.write(&frontPages, 2) != 2 || indexFile.write(&pad, 4) != 4) {
    error = "index write";
    return false;
  }
  return true;
}

bool TypesetBook::suspendIndex() {
  if (!indexFile) {
    return true;
  }
  if (!flushIndex()) {
    return false;
  }
  if (indexBase == kIndexPayload) {
    return finishIndex();
  }
  if (!writePartialHeader()) {
    return false;
  }
  const uint32_t at = static_cast<uint32_t>(indexBase) + static_cast<uint32_t>(nPages) * 7;
  if (!indexFile.seekSet(at) || !indexFile.sync()) {
    error = "index write";
    return false;
  }
  indexFile.probeContiguous();
  return true;
}

bool TypesetBook::promoteIndex() {
  if (indexBase != kPartialPayload || originAtom > 4 || frontPages != 0 || !ingestDone || !coversEnd) {
    return true;
  }
  if (!flushIndex()) {
    return false;
  }
  const uint32_t recBytes = static_cast<uint32_t>(nPages) * 7;
  HalFile work;
  if (!Storage.openFileForWrite("TS", kWorkIndex, work)) {
    error = "index write";
    return false;
  }
  if (!indexFile.seekSet(kPartialPayload) || !copyBytes(indexFile, work, recBytes)) {
    work.close();
    Storage.remove(kWorkIndex);
    error = "index write";
    return false;
  }
  work.close();
  if (!Storage.openFileForRead("TS", kWorkIndex, work)) {
    error = "index write";
    return false;
  }
  if (!indexFile.seekSet(kIndexPayload) || !copyBytes(work, indexFile, recBytes)) {
    work.close();
    Storage.remove(kWorkIndex);
    error = "index write";
    return false;
  }
  work.close();
  Storage.remove(kWorkIndex);
  if (!indexFile.truncate(static_cast<uint64_t>(kIndexPayload) + recBytes)) {
    error = "index write";
    return false;
  }
  const uint32_t magic = kIndexMagic;
  const uint32_t atomSz = sourceSize;
  const uint32_t srcSz = bookSrcSize;
  const uint16_t em = layoutOpt.em;
  const uint16_t w = static_cast<uint16_t>(layoutOpt.width);
  const uint16_t h = static_cast<uint16_t>(layoutOpt.height);
  const uint16_t modeWord = static_cast<uint16_t>(layoutOpt.mode);
  const uint32_t count = nPages;
  if (!indexFile.seekSet(0) || indexFile.write(&magic, 4) != 4 || indexFile.write(&atomSz, 4) != 4 ||
      indexFile.write(&srcSz, 4) != 4 || indexFile.write(&em, 2) != 2 || indexFile.write(&w, 2) != 2 ||
      indexFile.write(&h, 2) != 2 || indexFile.write(&modeWord, 2) != 2 || indexFile.write(&count, 4) != 4) {
    error = "index write";
    return false;
  }
  indexBase = kIndexPayload;
  partialOpen = false;
  char cpath[64];
  cursorPath(cpath, sizeof(cpath));
  Storage.remove(cpath);
  cursorOk = false;
  saveChapterSidecar();
  if (!indexFile.sync()) {
    error = "index write";
    return false;
  }
  LOG_INF("TS", "Index promoted %u pages", static_cast<unsigned>(nPages));
  return true;
}

bool TypesetBook::beginRange(const uint32_t origin, const uint16_t spine) {
  indexFile.close();
  nPages = 0;
  indexBufN = 0;
  char p[64];
  indexPath(p, sizeof(p));
  Storage.ensureDirectoryExists("/.crossjp");
  if (!Storage.openFileForWrite("TS", p, indexFile)) {
    error = "index file";
    return false;
  }
  originAtom = origin < 4 ? 4 : origin;
  indexedAtom = originAtom;
  originSpine = spine;
  coversEnd = false;
  pendingPage = false;
  frontFrozen = false;
  frontPages = 0;
  runMode = static_cast<uint16_t>(layoutOpt.mode);
  indexBase = static_cast<uint16_t>(kPartialPayload);
  if (!writePartialHeader()) {
    return false;
  }
  if (!appendPage(originAtom, static_cast<uint8_t>(layoutOpt.mode), 0) || !flushIndex()) {
    return false;
  }
  LOG_INF("TS", "Index range atom=%lu spine=%u", static_cast<unsigned long>(originAtom),
          static_cast<unsigned>(originSpine));
  return true;
}

bool TypesetBook::loadIndex() {
  indexFile.close();
  nPages = 0;
  indexBufN = 0;
  char p[64];
  indexPath(p, sizeof(p));
  if (!Storage.openFileForRead("TS", p, indexFile)) {
    return false;
  }
  uint32_t magic = 0;
  uint32_t atomSz = 0;
  uint32_t srcSz = 0;
  uint16_t em = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  uint16_t mode = 0;
  uint32_t count = 0;
  if (indexFile.read(&magic, 4) != 4 || (magic != kIndexMagic && magic != kIndexPartial) ||
      indexFile.read(&atomSz, 4) != 4 || indexFile.read(&srcSz, 4) != 4 || srcSz != bookSrcSize ||
      indexFile.read(&em, 2) != 2 || em != layoutOpt.em || indexFile.read(&w, 2) != 2 ||
      w != static_cast<uint16_t>(layoutOpt.width) || indexFile.read(&h, 2) != 2 ||
      h != static_cast<uint16_t>(layoutOpt.height) || indexFile.read(&mode, 2) != 2 ||
      indexFile.read(&count, 4) != 4 || count == 0 || count > kMaxPages) {
    indexFile.close();
    return false;
  }
  if (magic == kIndexMagic) {
    if (atomSz != sourceSize || indexFile.fileSize() < kIndexPayload + count * 7) {
      indexFile.close();
      return false;
    }
    layoutOpt.mode = static_cast<ts::WritingMode>(mode);
    runMode = mode;
    nPages = static_cast<uint16_t>(count);
    originAtom = 4;
    indexedAtom = sourceSize;
    frontPages = 0;
    frontFrozen = true;
    ingestDone = true;
    coversEnd = true;
    pendingPage = false;
    indexBase = static_cast<uint16_t>(kIndexPayload);
    partialOpen = false;
    indexFile.probeContiguous();
    LOG_INF("TS", "Index %s (%u pages)", p, static_cast<unsigned>(count));
    return true;
  }

  uint32_t origin = 0;
  uint32_t indexed = 0;
  uint32_t flags = 0;
  uint16_t next = 0;
  uint16_t spines = 0;
  uint16_t oSpine = 0;
  uint16_t front = 0;
  uint32_t pad = 0;
  if (indexFile.read(&origin, 4) != 4 || indexFile.read(&indexed, 4) != 4 || indexFile.read(&flags, 4) != 4 ||
      indexFile.read(&next, 2) != 2 || indexFile.read(&spines, 2) != 2 || indexFile.read(&oSpine, 2) != 2 ||
      indexFile.read(&front, 2) != 2 || indexFile.read(&pad, 4) != 4 || origin < 4 || indexed < origin ||
      indexed > sourceSize || indexFile.fileSize() < kPartialPayload + count * 7) {
    indexFile.close();
    return false;
  }
  layoutOpt.mode = static_cast<ts::WritingMode>(mode);
  nPages = static_cast<uint16_t>(count);
  originAtom = origin;
  indexedAtom = indexed;
  nextSpine = next;
  if (spines > 0) {
    spineCount = spines;
  }
  originSpine = oSpine;
  frontPages = front;
  runMode = static_cast<uint16_t>(pad);
  ingestDone = (flags & kFlagIngest) != 0;
  coversEnd = (flags & kFlagCovers) != 0;
  pendingPage = (flags & kFlagPending) != 0;
  frontFrozen = (flags & kFlagFrozen) != 0;
  if (originAtom <= 4) {
    frontPages = 0;
    frontFrozen = true;
  }
  // coversEnd means the index has reached the end of a finished ingest.
  // A prefix that ran out of atoms still has later spines to append.
  if (!ingestDone || sourceSize > indexedAtom) {
    coversEnd = false;
  }
  indexBase = static_cast<uint16_t>(kPartialPayload);
  if (!cursorOk && spineCount > 0 && nextSpine >= spineCount) {
    ingestDone = true;
  }
  partialOpen = !buildFinished();

  HalFile rw = Storage.open(p, O_RDWR);
  if (!rw) {
    extendFailed = true;
    indexFile.probeContiguous();
    LOG_ERR("TS", "Index read-only %s", p);
    return true;
  }
  indexFile.close();
  indexFile = std::move(rw);
  const uint32_t at = kPartialPayload + static_cast<uint32_t>(nPages) * 7;
  if (!indexFile.seekSet(at)) {
    error = "index file";
    indexFile.close();
    return false;
  }
  if (buildFinished() && !promoteIndex()) {
    return false;
  }
  LOG_INF("TS", "Index %s (%u pages, partial=%d)", p, static_cast<unsigned>(nPages), partialOpen ? 1 : 0);
  return true;
}

TypesetBook::IndexStep TypesetBook::indexMore(const uint16_t budget, const uint32_t stopAtom) {
  if (budget == 0) {
    return IndexStep::Ok;
  }
  if (!atomsLive && !reopenAtoms()) {
    return IndexStep::Error;
  }
  if (!indexFile) {
    error = "index file";
    return IndexStep::Error;
  }
  if (!flushIndex()) {
    return IndexStep::Error;
  }
  const uint32_t appendAt = static_cast<uint32_t>(indexBase) + static_cast<uint32_t>(nPages) * 7;
  if (!indexFile.seekSet(appendAt)) {
    error = "index write";
    return IndexStep::Error;
  }
  if (!atoms.seek(indexedAtom)) {
    error = "atom open";
    return IndexStep::Error;
  }

  ts::WritingMode cur = static_cast<ts::WritingMode>(runMode);
  if (!pendingPage && nPages > 0) {
    uint32_t off = 0;
    uint8_t mode = 0;
    uint16_t pic = 0;
    if (readPage(nPages - 1, off, mode, pic)) {
      cur = static_cast<ts::WritingMode>(mode);
    }
    if (!indexFile.seekSet(appendAt)) {
      error = "index write";
      return IndexStep::Error;
    }
  }

  ts::LayoutOptions pageOpt = layoutOpt;
  pageOpt.mode = cur;
  layouter.begin(pageOpt);
  bool pendingStart = pendingPage;
  uint16_t closed = 0;

  // The layouter is not written to the card. Stopping mid-page rewinds to that
  // page's first atom so the next call replays it and does not emit a second record.
  auto park = [&]() {
    if (layouter.currentGlyphCount() > 0) {
      indexedAtom = layouter.currentPagePos();
      pendingPage = false;
    } else {
      indexedAtom = atoms.position();
      pendingPage = pendingStart;
    }
    runMode = static_cast<uint16_t>(cur);
  };

  while (closed < budget) {
    if (stopAtom != 0 && atoms.position() >= stopAtom) {
      indexedAtom = stopAtom;
      pendingPage = true;
      runMode = static_cast<uint16_t>(cur);
      if (!flushIndex()) {
        return IndexStep::Error;
      }
      return IndexStep::Ok;
    }
    const uint32_t atomPos = atoms.position();
    ts::Atom atom{};
    if (!atoms.next(atom)) {
      const bool cleanEof = atomBytes < 4 || atoms.position() >= atomBytes;
      if (!ingestDone && cleanEof) {
        park();
        if (!flushIndex()) {
          return IndexStep::Error;
        }
        return IndexStep::NeedAtoms;
      }
      if (!ingestDone && !cleanEof) {
        error = "atom open";
        return IndexStep::Error;
      }
      // More spines would still append atoms, so this is not layouter.finish().
      coversEnd = true;
      indexedAtom = atoms.position();
      pendingPage = false;
      runMode = static_cast<uint16_t>(cur);
      if (!flushIndex()) {
        return IndexStep::Error;
      }
      if (!promoteIndex()) {
        return IndexStep::Error;
      }
      return IndexStep::Done;
    }
    const bool isMode = atom.kind == ts::AtomKind::Mode;
    const ts::WritingMode nextMode =
        isMode ? (atom.cp == 1 ? ts::WritingMode::HorizontalTb : ts::WritingMode::VerticalRl) : cur;
    const bool complete = layouter.feed(atom, atomPos);
    if (pendingStart && layouter.currentGlyphCount() > 0) {
      if (!appendPage(layouter.currentPagePos(), static_cast<uint8_t>(cur), 0)) {
        return IndexStep::Error;
      }
      pendingStart = false;
    }
    if (isMode) {
      cur = nextMode;
    }
    if (!complete) {
      continue;
    }
    layouter.clearPage();
    const uint16_t taken = layouter.takenPicture();
    const uint16_t deferred = layouter.deferredPicture();
    if (taken != 0) {
      if (pendingStart) {
        if (!appendPage(atomPos, static_cast<uint8_t>(cur), taken)) {
          return IndexStep::Error;
        }
      } else if (!setLastPicture(taken)) {
        return IndexStep::Error;
      }
      pendingStart = true;
    } else if (deferred != 0) {
      if (!appendPage(atomPos, static_cast<uint8_t>(cur), deferred)) {
        return IndexStep::Error;
      }
      pendingStart = true;
    } else if (layouter.currentGlyphCount() > 0) {
      if (!appendPage(layouter.currentPagePos(), static_cast<uint8_t>(cur), 0)) {
        return IndexStep::Error;
      }
      pendingStart = false;
    } else {
      pendingStart = true;
    }
    ++closed;
    yield();
  }
  park();
  if (!flushIndex()) {
    return IndexStep::Error;
  }
  return IndexStep::Ok;
}

bool TypesetBook::indexUntil(const uint32_t target, ts::EpubBook::ProgressFn progress, void* progressCtx) {
  int idle = 0;
  while (!pageCovers(target) && !buildFinished() && idle < 4) {
    const uint16_t pages0 = nPages;
    const uint16_t spine0 = nextSpine;
    const uint32_t indexed0 = indexedAtom;
    const IndexStep step = indexMore(8, 0);
    if (step == IndexStep::Error) {
      if (!frontFrozen) {
        freezeFront();
      }
      suspendIndex();
      return false;
    }
    if (step == IndexStep::NeedAtoms) {
      if (!ingestSlice(1, progress, progressCtx)) {
        if (!frontFrozen) {
          freezeFront();
        }
        suspendIndex();
        return false;
      }
    } else if (step == IndexStep::Done) {
      break;
    }
    if (nPages == pages0 && nextSpine == spine0 && indexedAtom == indexed0) {
      ++idle;
    } else {
      idle = 0;
    }
  }
  if (!frontFrozen) {
    freezeFront();
  }
  suspendIndex();
  return pageCovers(target) || buildFinished();
}

bool TypesetBook::prependOne() {
  if (originAtom <= 4 || nPages == 0 || !indexFile) {
    return false;
  }
  uint32_t prev = 4;
  uint16_t prevSpine = 0;
  bool found = false;
  for (const auto& m : chapterMarks) {
    if (m.atomOff == 0xFFFFFFFFu || m.atomOff >= originAtom || m.atomOff < 4) {
      continue;
    }
    if (!found || m.atomOff > prev) {
      prev = m.atomOff;
      prevSpine = m.spine == 0xFFFFu ? static_cast<uint16_t>(0) : m.spine;
      found = true;
    }
  }
  if (!flushIndex()) {
    return false;
  }
  const uint16_t oldPages = nPages;
  const uint32_t oldOrigin = originAtom;
  const uint16_t oldSpine = originSpine;
  const uint16_t oldFront = frontPages;
  const bool oldFrozen = frontFrozen;
  const uint32_t oldIndexed = indexedAtom;
  const bool oldCovers = coversEnd;
  const bool oldPending = pendingPage;
  const uint16_t oldRun = runMode;
  const uint32_t recBytes = static_cast<uint32_t>(oldPages) * 7;

  HalFile work;
  if (!Storage.openFileForWrite("TS", kWorkIndex, work)) {
    error = "index write";
    return false;
  }
  if (!indexFile.seekSet(indexBase) || !copyBytes(indexFile, work, recBytes)) {
    work.close();
    Storage.remove(kWorkIndex);
    error = "index write";
    indexFile.seekSet(static_cast<uint32_t>(indexBase) + recBytes);
    return false;
  }
  work.close();

  auto restore = [&]() -> bool {
    indexFile.close();
    nPages = 0;
    indexBufN = 0;
    indexBase = static_cast<uint16_t>(kPartialPayload);
    originAtom = oldOrigin;
    originSpine = oldSpine;
    indexedAtom = oldIndexed;
    coversEnd = oldCovers;
    pendingPage = oldPending;
    frontPages = oldFront;
    frontFrozen = oldFrozen;
    runMode = oldRun;
    char p[64];
    indexPath(p, sizeof(p));
    if (!Storage.openFileForWrite("TS", p, indexFile) || !writePartialHeader()) {
      return false;
    }
    HalFile in;
    if (!Storage.openFileForRead("TS", kWorkIndex, in)) {
      return false;
    }
    const bool copied = copyBytes(in, indexFile, recBytes);
    in.close();
    if (!copied) {
      return false;
    }
    nPages = oldPages;
    return suspendIndex();
  };

  if (!beginRange(prev, prevSpine)) {
    restore();
    return false;
  }
  const IndexStep step = indexMore(kMaxPages, oldOrigin);
  if (step == IndexStep::Error || step == IndexStep::NeedAtoms) {
    restore();
    return false;
  }
  const uint16_t added = nPages;
  if (static_cast<uint32_t>(added) + oldPages > kMaxPages) {
    error = "too many pages";
    restore();
    return false;
  }
  if (!flushIndex()) {
    restore();
    return false;
  }
  if (!Storage.openFileForRead("TS", kWorkIndex, work) || !copyBytes(work, indexFile, recBytes)) {
    work.close();
    error = "index write";
    restore();
    return false;
  }
  work.close();
  Storage.remove(kWorkIndex);
  nPages = static_cast<uint16_t>(static_cast<uint32_t>(added) + oldPages);
  indexedAtom = oldIndexed;
  coversEnd = oldCovers;
  pendingPage = oldPending;
  runMode = oldRun;
  if (originAtom <= 4) {
    frontPages = 0;
    frontFrozen = true;
  } else if (oldFront > added) {
    frontPages = static_cast<uint16_t>(oldFront - added);
    frontFrozen = true;
  } else {
    frontPages = 1;
    frontFrozen = true;
  }
  LOG_INF("TS", "Prepended %u pages, front=%u", static_cast<unsigned>(added), static_cast<unsigned>(frontPages));
  return suspendIndex();
}

bool TypesetBook::extendAhead(const uint32_t globalPage, const uint16_t ahead) {
  if (!opened || ahead == 0 || buildFinished() || extendFailed || (coversEnd && ingestDone)) {
    return true;
  }
  const uint32_t have = static_cast<uint32_t>(frontPages) + nPages;
  if (globalPage >= frontPages && globalPage + ahead < have) {
    return true;
  }
  const uint16_t budget = ahead > 8 ? 8 : ahead;
  const IndexStep step = indexMore(budget, 0);
  if (step == IndexStep::Error) {
    extendFailed = true;
    LOG_ERR("TS", "Extend stopped: %s", error ? error : "");
    return false;
  }
  if (step == IndexStep::NeedAtoms) {
    if (!ingestSlice(1, nullptr, nullptr)) {
      extendFailed = true;
      LOG_ERR("TS", "Extend stopped: %s", error ? error : "");
      return false;
    }
  }
  if (!frontFrozen) {
    freezeFront();
  }
  return suspendIndex();
}

bool TypesetBook::ensureGlobal(const uint32_t globalPage, ts::EpubBook::ProgressFn progress, void* progressCtx) {
  if (!opened || nPages == 0) {
    return false;
  }
  int guard = 0;
  while (globalPage < frontPages && originAtom > 4 && guard < 512) {
    const uint32_t before = originAtom;
    if (!prependOne()) {
      return false;
    }
    if (originAtom >= before) {
      return false;
    }
    ++guard;
  }
  guard = 0;
  int idle = 0;
  while (globalPage >= static_cast<uint32_t>(frontPages) + nPages && !buildFinished() && idle < 4 && guard < 8000) {
    const uint16_t pages0 = nPages;
    const uint16_t spine0 = nextSpine;
    const IndexStep step = indexMore(8, 0);
    if (step == IndexStep::Error) {
      return false;
    }
    if (step == IndexStep::NeedAtoms) {
      if (!ingestSlice(1, progress, progressCtx)) {
        break;
      }
    } else if (step == IndexStep::Done) {
      break;
    }
    if (nPages == pages0 && nextSpine == spine0) {
      ++idle;
    } else {
      idle = 0;
    }
    ++guard;
  }
  if (!frontFrozen) {
    freezeFront();
  }
  suspendIndex();
  return globalPage < static_cast<uint32_t>(frontPages) + nPages;
}

bool TypesetBook::ensureChapter(const uint16_t chapterIndex, uint32_t& globalPage, ts::EpubBook::ProgressFn progress,
                               void* progressCtx) {
  if (!opened || chapterIndex >= chapterMarks.size()) {
    return false;
  }
  int guard = 0;
  while (chapterMarks[chapterIndex].atomOff == 0xFFFFFFFFu && !ingestDone && guard < 512) {
    const uint16_t sp = chapterMarks[chapterIndex].spine;
    if (sp != 0xFFFFu && nextSpine > sp) {
      break;
    }
    const uint16_t before = nextSpine;
    if (!ingestSlice(1, progress, progressCtx)) {
      return false;
    }
    if (nextSpine == before) {
      break;
    }
    ++guard;
  }
  const uint32_t atom = chapterMarks[chapterIndex].atomOff;
  if (atom == 0xFFFFFFFFu) {
    globalPage = frontPages;
    return false;
  }
  guard = 0;
  while (atom < originAtom && originAtom > 4 && guard < 512) {
    const uint32_t before = originAtom;
    if (!prependOne()) {
      return false;
    }
    if (originAtom >= before) {
      return false;
    }
    ++guard;
  }
  if (!pageCovers(atom) && !indexUntil(atom, progress, progressCtx)) {
    globalPage = frontPages;
    return false;
  }
  if (!frontFrozen) {
    freezeFront();
  }
  globalPage = globalForAtom(atom);
  applyChapters();
  suspendIndex();
  return true;
}
