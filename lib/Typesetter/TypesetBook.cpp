#include "TypesetBook.h"

#include "PicturePage.h"

#include <BoardConfig.h>
#include <Gfx.h>
#include <Logging.h>
#include <ScratchHeap.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

// TID7: header, then one 7-byte record per page (offset, mode, picture).
// TID6 kept three RAM vectors and operator new aborted on a long series.
constexpr uint32_t kIndexMagic = 0x37444954u;
constexpr uint32_t kIndexPayload = 24;
constexpr uint16_t kMaxPages = 65535;

void putU32(uint8_t* d, const uint32_t v) {
  d[0] = static_cast<uint8_t>(v);
  d[1] = static_cast<uint8_t>(v >> 8);
  d[2] = static_cast<uint8_t>(v >> 16);
  d[3] = static_cast<uint8_t>(v >> 24);
}

// A cover-only EPUB is a few picture atoms, well under the broken-ingest floor.
bool pictureSidecarFresh(const char* path, const uint16_t pageW, const uint16_t pageH) {
  HalFile f;
  uint8_t hdr[10];
  if (!path || !path[0] || pageW == 0 || pageH == 0 || !Storage.openFileForRead("TS", path, f) ||
      f.read(hdr, sizeof(hdr)) != static_cast<int>(sizeof(hdr))) {
    return false;
  }
  const uint32_t magic = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8) |
                         (static_cast<uint32_t>(hdr[2]) << 16) | (static_cast<uint32_t>(hdr[3]) << 24);
  const uint16_t count = static_cast<uint16_t>(hdr[4] | (hdr[5] << 8));
  const uint16_t w = static_cast<uint16_t>(hdr[6] | (hdr[7] << 8));
  const uint16_t h = static_cast<uint16_t>(hdr[8] | (hdr[9] << 8));
  return magic == ts::kPictureMagic && count > 0 && w == pageW && h == pageH;
}

uint32_t pathHash(const char* path) {
  uint32_t h = 2166136261u;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

uint32_t fileSizeOf(const char* path) {
  HalFile f;
  if (!path || !Storage.openFileForRead("TS", path, f)) {
    return 0;
  }
  return static_cast<uint32_t>(f.fileSize());
}

void atomCacheName(char* out, const size_t outSize, const unsigned long hash, const char tag) {
  snprintf(out, outSize, "/.crossjp/%c_%08lx.bin", tag, hash);
}

// Truncate or create `path`. A stuck exFAT directory entry (bitmap already
// clear, or dataLength past the allocated run) makes O_TRUNC fail in one
// sector even though the file still opens for read. Recover the card, delete,
// and try once more.
bool replaceAtomFile(const char* path) {
  HalFile f;
  if (Storage.openFileForWrite("TS", path, f)) {
    f.close();
    return true;
  }
  uint8_t sdErr = 0;
  Storage.recoverCard(&sdErr);
  const bool removed = Storage.remove(path);
  if (Storage.openFileForWrite("TS", path, f)) {
    f.close();
    LOG_INF("TS", "Replaced atom %s sd=0x%02X", path, sdErr);
    return true;
  }
  uint64_t freeB = 0;
  if (Storage.freeBytes(&freeB)) {
    LOG_ERR("TS", "Atom blocked %s sd=0x%02X removed=%d free=%lu KB", path, sdErr, removed ? 1 : 0,
            static_cast<unsigned long>(freeB / 1024));
  } else {
    LOG_ERR("TS", "Atom blocked %s sd=0x%02X removed=%d", path, sdErr, removed ? 1 : 0);
  }
  return false;
}

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

void copyBasename(char* out, const size_t outSize, const char* path) {
  const char* slash = strrchr(path, '/');
  const char* name = slash ? slash + 1 : path;
  snprintf(out, outSize, "%s", name);
  char* dot = strrchr(out, '.');
  if (dot) {
    *dot = '\0';
  }
}

}  // namespace

bool TypesetBook::hasBookExt(const char* path) {
  return path && (endsWithI(path, ".txt") || endsWithI(path, ".epub"));
}

void TypesetBook::indexPath(char* out, const size_t outSize) const {
  snprintf(out, outSize, "/.crossjp/t_%08lx.bin", static_cast<unsigned long>(pathHash(filepath)));
}

void TypesetBook::chapterPath(char* out, const size_t outSize) const {
  snprintf(out, outSize, "/.crossjp/c_%08lx.bin", static_cast<unsigned long>(pathHash(filepath)));
}

bool TypesetBook::loadFont(const char* fontPath) {
  if (!fontPath || fontPath[0] == '\0') {
    error = "font missing";
    return false;
  }
  LOG_INF("TS", "Font %s", fontPath);
  if (!font.load(fontPath)) {
    error = font.lastError()[0] ? font.lastError() : "font missing";
    return false;
  }
  return true;
}

bool TypesetBook::open(const char* path, ts::EpubBook::ProgressFn progress, void* progressCtx,
                      const char* fontPath) {
  close();
  if (!path || path[0] == '\0') {
    error = "no path";
    return false;
  }
  if (!hasBookExt(path)) {
    error = "unsupported book";
    return false;
  }
  snprintf(filepath, sizeof(filepath), "%s", path);
  copyBasename(bookTitle, sizeof(bookTitle), filepath);

  const unsigned long t0 = millis();
  ScratchHeap::release();

  const unsigned long tFont = millis();
  if (!loadFont(fontPath)) {
    LOG_ERR("TS", "Font load failed: %s", error);
    return false;
  }
  LOG_INF("TS", "time font %lums", millis() - tFont);

  layoutOpt.width = static_cast<int16_t>(BoardConfig::ACTIVE.displayHeight);
  layoutOpt.height = static_cast<int16_t>(BoardConfig::ACTIVE.displayWidth);
  layoutOpt.em = font.emPx();
  layoutOpt.margin = 0;
  layoutOpt.hasRuby = true;
  layoutOpt.compactColumns = false;
  layoutOpt.mode = ts::WritingMode::VerticalRl;

  const unsigned long hash = static_cast<unsigned long>(pathHash(filepath));
  char primary[64];
  char alt[64];
  atomCacheName(primary, sizeof(primary), hash, 'a');
  atomCacheName(alt, sizeof(alt), hash, 'b');
  snprintf(picturePath, sizeof(picturePath), "/.crossjp/i_%08lx.bin", hash);
  Storage.ensureDirectoryExists("/.crossjp");

  bookSrcSize = fileSizeOf(filepath);
  const uint32_t primarySize = fileSizeOf(primary);
  snprintf(atomPath, sizeof(atomPath), "%s", primary);
  sourceSize = primarySize;
  const bool epub = endsWithI(filepath, ".epub");
  bool fresh = atomCacheFresh();
  if (!fresh && Storage.exists(alt)) {
    snprintf(atomPath, sizeof(atomPath), "%s", alt);
    sourceSize = fileSizeOf(alt);
    fresh = atomCacheFresh();
    if (!fresh) {
      snprintf(atomPath, sizeof(atomPath), "%s", primary);
      sourceSize = primarySize;
    }
  }
  if (epub && fresh && !loadChapterSidecar()) {
    fresh = false;
    LOG_INF("TS", "Reingest for chapter TOC");
  }
  if (fresh) {
    LOG_INF("TS", "time ingest 0ms skip (atom cache)");
  } else {
    // The reading font stays open across ingest otherwise, and the atom
    // create shares the card with that multi-megabyte read.
    font.releaseFile();
    if (!replaceAtomFile(atomPath)) {
      const char* other = strcmp(atomPath, primary) == 0 ? alt : primary;
      if (replaceAtomFile(other)) {
        snprintf(atomPath, sizeof(atomPath), "%s", other);
        LOG_INF("TS", "Atom cache %s", atomPath);
      } else {
        error = "atom file";
        close();
        return false;
      }
    }
    const unsigned long tIngest = millis();
    if (epub) {
      if (!ingestEpub(progress, progressCtx)) {
        close();
        return false;
      }
    } else if (!ingestTxt()) {
      close();
      return false;
    }
    sourceSize = fileSizeOf(atomPath);
    LOG_INF("TS", "time ingest %lums", millis() - tIngest);
  }

  if (!atoms.open(atomPath)) {
    error = "atom open";
    close();
    return false;
  }

  const unsigned long tIdx = millis();
  const bool cached = loadIndex();
  if (!cached && !buildIndex()) {
    close();
    return false;
  }
  LOG_INF("TS", "time index %lums %s pages=%u", millis() - tIdx, cached ? "cache" : "build", pageCount());
  applyChapters();
  opened = true;
  error = "";
  LOG_INF("TS", "Open %s pages=%u em=%u chapters=%u total %lums", filepath, pageCount(), layoutOpt.em,
          static_cast<unsigned>(chapters.size()), millis() - t0);
  return true;
}

void TypesetBook::close() {
  opened = false;
  atoms.close();
  font.close();
  indexFile.close();
  nPages = 0;
  indexBufN = 0;
  chapters.clear();
  chapterMarks.clear();
  sourceSize = 0;
  bookSrcSize = 0;
  loadedPage = 0xFFFFFFFFu;
  loadedCount = 0;
  loadedPicture = 0;
  pictureBits = nullptr;
  cleanupPending = false;
  ScratchHeap::reserve(BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
}

bool TypesetBook::ingestTxt() {
  font.releaseFile();
  HalFile in;
  if (!Storage.openFileForRead("TS", filepath, in)) {
    error = "text missing";
    return false;
  }
  in.probeContiguous();
  ts::Utf8AtomReader r;
  r.bind(&in);
  ts::AtomWriter w;
  if (!w.open(atomPath)) {
    error = "atom write";
    return false;
  }
  ts::Atom a{};
  uint32_t pos = 0;
  while (r.next(a, pos)) {
    if (!w.write(a)) {
      error = w.diskFull() ? "disk full" : "atom write";
      w.close();
      if (atomPath[0]) {
        Storage.remove(atomPath);
      }
      return false;
    }
  }
  w.close();
  if (!font.reopenFile()) {
    error = "font maps";
    return false;
  }
  return true;
}

bool TypesetBook::ingestEpub(ts::EpubBook::ProgressFn progress, void* progressCtx) {
  font.releaseMaps();
  font.releaseFile();
  ts::EpubBook epub;
  const bool ok = epub.open(filepath, atomPath, progress, progressCtx, static_cast<uint16_t>(layoutOpt.width),
                            static_cast<uint16_t>(layoutOpt.height), picturePath);
  if (ok) {
    if (epub.title()[0]) {
      snprintf(bookTitle, sizeof(bookTitle), "%s", epub.title());
    }
    layoutOpt.mode = epub.bookMode();
    chapterMarks.clear();
    chapterMarks.reserve(epub.tocCount());
    for (uint16_t i = 0; i < epub.tocCount(); ++i) {
      const auto& t = epub.toc(i);
      if (t.atomOff == 0xFFFFFFFFu || t.title[0] == 0) {
        continue;
      }
      ChapterMark m;
      m.atomOff = t.atomOff;
      snprintf(m.name, sizeof(m.name), "%s", t.title);
      chapterMarks.push_back(m);
    }
    saveChapterSidecar();
  }
  const char* epubError = epub.lastError();
  // The zip table and the spine sit in the same hole the font cmap needs.
  epub.close();
  if (!font.reopenFile() || !font.restoreMaps()) {
    error = "font maps";
    return false;
  }
  if (!ok) {
    error = epubError && epubError[0] ? epubError : "epub";
    return false;
  }
  return true;
}

bool TypesetBook::saveChapterSidecar() const {
  char p[64];
  chapterPath(p, sizeof(p));
  HalFile f;
  if (!Storage.openFileForWrite("TS", p, f)) {
    return false;
  }
  const uint32_t magic = 0x33544843u;  // CHT3
  const uint16_t n = static_cast<uint16_t>(chapterMarks.size());
  f.write(&magic, 4);
  f.write(&n, 2);
  for (uint16_t i = 0; i < n; ++i) {
    f.write(&chapterMarks[i].atomOff, 4);
    const uint8_t len = static_cast<uint8_t>(strlen(chapterMarks[i].name));
    f.write(&len, 1);
    if (len > 0) {
      f.write(chapterMarks[i].name, len);
    }
  }
  return true;
}

bool TypesetBook::loadChapterSidecar() {
  char p[64];
  chapterPath(p, sizeof(p));
  HalFile f;
  if (!Storage.openFileForRead("TS", p, f)) {
    return false;
  }
  uint32_t magic = 0;
  uint16_t n = 0;
  if (f.read(&magic, 4) != 4 || magic != 0x33544843u || f.read(&n, 2) != 2 || n > 512) {
    return false;
  }
  chapterMarks.clear();
  chapterMarks.reserve(n);
  for (uint16_t i = 0; i < n; ++i) {
    ChapterMark m;
    uint8_t len = 0;
    if (f.read(&m.atomOff, 4) != 4 || f.read(&len, 1) != 1 || len >= sizeof(m.name)) {
      chapterMarks.clear();
      return false;
    }
    if (len > 0 && f.read(m.name, len) != static_cast<int>(len)) {
      chapterMarks.clear();
      return false;
    }
    m.name[len] = 0;
    chapterMarks.push_back(m);
  }
  return true;
}

void TypesetBook::applyChapters() {
  chapters.clear();
  if (chapterMarks.empty() || nPages == 0 || !indexFile) {
    return;
  }
  chapters.reserve(chapterMarks.size());
  for (const auto& m : chapterMarks) {
    ts::ChapterInfo ch;
    ch.name = m.name;
    chapters.push_back(std::move(ch));
  }
  std::vector<uint16_t> order(chapterMarks.size());
  for (uint16_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](const uint16_t a, const uint16_t b) {
    return chapterMarks[a].atomOff < chapterMarks[b].atomOff;
  });
  if (!indexFile.seekSet(kIndexPayload)) {
    return;
  }
  uint8_t buf[7 * 32];
  uint16_t page = 0;
  size_t oi = 0;
  while (page < nPages && oi < order.size()) {
    const uint16_t room = static_cast<uint16_t>(nPages - page);
    const uint16_t want = room > 32 ? 32 : room;
    const int got = indexFile.read(buf, static_cast<size_t>(want) * 7);
    if (got < 7) {
      break;
    }
    const uint16_t nrec = static_cast<uint16_t>(static_cast<unsigned>(got) / 7);
    for (uint16_t r = 0; r < nrec && oi < order.size(); ++r) {
      const uint8_t* b = buf + static_cast<size_t>(r) * 7;
      const uint32_t off = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                           (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
      while (oi < order.size() && chapterMarks[order[oi]].atomOff < off) {
        chapters[order[oi]].startPage = page > 0 ? static_cast<uint16_t>(page - 1) : 0;
        ++oi;
      }
      ++page;
    }
  }
  while (oi < order.size()) {
    chapters[order[oi]].startPage = static_cast<uint16_t>(nPages - 1);
    ++oi;
  }
  const uint16_t last = static_cast<uint16_t>(nPages - 1);
  for (size_t i = 0; i < chapters.size(); ++i) {
    const uint16_t next = (i + 1 < chapters.size()) ? chapters[i + 1].startPage : static_cast<uint16_t>(last + 1);
    chapters[i].endPage = next > chapters[i].startPage ? static_cast<uint16_t>(next - 1) : chapters[i].startPage;
  }
  LOG_INF("TS", "Chapters %u", static_cast<unsigned>(chapters.size()));
}

bool TypesetBook::atomCacheFresh() const {
  if (sourceSize == 0) {
    return false;
  }
  {
    HalFile atom;
    uint32_t magic = 0;
    if (!Storage.openFileForRead("TS", atomPath, atom) || atom.read(&magic, 4) != 4 || magic != ts::kAtomMagic) {
      return false;  // pre-IRA6 ingest; picture pages changed the atom stream
    }
  }
  // A cache-clear used to leave a file of page-breaks, about one byte each.
  // A real cover book is that small too, and its PIC1 header says so.
  if (endsWithI(filepath, ".epub") && sourceSize < 2048 &&
      !pictureSidecarFresh(picturePath, static_cast<uint16_t>(layoutOpt.width),
                           static_cast<uint16_t>(layoutOpt.height))) {
    return false;
  }
  char p[64];
  indexPath(p, sizeof(p));
  HalFile idx;
  if (!Storage.openFileForRead("TS", p, idx)) {
    return true;  // atoms exist; index will be rebuilt
  }
  uint32_t magic = 0;
  uint32_t atomSz = 0;
  uint32_t srcSz = 0;
  if (idx.read(&magic, 4) != 4) {
    return true;
  }
  if (magic == kIndexMagic) {
    if (idx.read(&atomSz, 4) != 4 || atomSz != sourceSize) {
      return false;
    }
    if (idx.read(&srcSz, 4) != 4 || (bookSrcSize != 0 && srcSz != bookSrcSize)) {
      return false;  // EPUB replaced
    }
  }
  return true;
}

bool TypesetBook::readPage(const uint32_t index, uint32_t& off, uint8_t& mode, uint16_t& pic) const {
  if (index >= nPages || !indexFile) {
    return false;
  }
  uint8_t b[7];
  const uint32_t at = kIndexPayload + index * 7;
  if (!indexFile.seekSet(at) || indexFile.read(b, 7) != 7) {
    return false;
  }
  off = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
        (static_cast<uint32_t>(b[3]) << 24);
  mode = b[4];
  pic = static_cast<uint16_t>(b[5] | (b[6] << 8));
  return true;
}

bool TypesetBook::flushIndex() {
  if (indexBufN == 0) {
    return true;
  }
  if (indexFile.write(indexBuf, indexBufN) != indexBufN) {
    error = "index write";
    return false;
  }
  indexBufN = 0;
  return true;
}

bool TypesetBook::appendPage(const uint32_t off, const uint8_t mode, const uint16_t pic) {
  if (nPages >= kMaxPages) {
    error = "too many pages";
    return false;
  }
  if (indexBufN + 7 > sizeof(indexBuf) && !flushIndex()) {
    return false;
  }
  // Records are 7 bytes, so the offset is not aligned. Store it little-endian.
  uint8_t* d = indexBuf + indexBufN;
  putU32(d, off);
  d[4] = mode;
  d[5] = static_cast<uint8_t>(pic);
  d[6] = static_cast<uint8_t>(pic >> 8);
  indexBufN = static_cast<uint16_t>(indexBufN + 7);
  ++nPages;
  if ((nPages % 1024) == 0) {
    LOG_INF("TS", "index %u", nPages);
  }
  return true;
}

bool TypesetBook::setLastPicture(const uint16_t pic) {
  if (nPages == 0) {
    return true;
  }
  if (indexBufN >= 7) {
    indexBuf[indexBufN - 2] = static_cast<uint8_t>(pic);
    indexBuf[indexBufN - 1] = static_cast<uint8_t>(pic >> 8);
    return true;
  }
  const uint32_t end = kIndexPayload + static_cast<uint32_t>(nPages) * 7;
  const uint8_t b[2] = {static_cast<uint8_t>(pic), static_cast<uint8_t>(pic >> 8)};
  if (!indexFile.seekSet(end - 2) || indexFile.write(b, 2) != 2 || !indexFile.seekSet(end)) {
    error = "index write";
    return false;
  }
  return true;
}

bool TypesetBook::finishIndex() {
  if (!flushIndex()) {
    return false;
  }
  const uint32_t count = nPages;
  if (!indexFile.seekSet(20) || indexFile.write(&count, 4) != 4 || !indexFile.sync()) {
    error = "index write";
    return false;
  }
  indexFile.probeContiguous();
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
  const bool headerOk = indexFile.read(&magic, 4) == 4 && magic == kIndexMagic && indexFile.read(&atomSz, 4) == 4 &&
                        atomSz == sourceSize && indexFile.read(&srcSz, 4) == 4 && srcSz == bookSrcSize &&
                        indexFile.read(&em, 2) == 2 && em == layoutOpt.em && indexFile.read(&w, 2) == 2 &&
                        w == static_cast<uint16_t>(layoutOpt.width) && indexFile.read(&h, 2) == 2 &&
                        h == static_cast<uint16_t>(layoutOpt.height) && indexFile.read(&mode, 2) == 2 &&
                        indexFile.read(&count, 4) == 4 && count > 0 && count <= kMaxPages &&
                        indexFile.fileSize() >= kIndexPayload + count * 7;
  if (!headerOk) {
    indexFile.close();
    return false;
  }
  layoutOpt.mode = static_cast<ts::WritingMode>(mode);
  nPages = static_cast<uint16_t>(count);
  indexFile.probeContiguous();
  LOG_INF("TS", "Index %s (%u pages)", p, static_cast<unsigned>(count));
  return true;
}

bool TypesetBook::buildIndex() {
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
  const uint32_t magic = kIndexMagic;
  const uint32_t atomSz = sourceSize;
  const uint32_t srcSz = bookSrcSize;
  const uint16_t em = layoutOpt.em;
  const uint16_t w = static_cast<uint16_t>(layoutOpt.width);
  const uint16_t h = static_cast<uint16_t>(layoutOpt.height);
  const uint16_t modeWord = static_cast<uint16_t>(layoutOpt.mode);
  const uint32_t count = 0;
  if (indexFile.write(&magic, 4) != 4 || indexFile.write(&atomSz, 4) != 4 || indexFile.write(&srcSz, 4) != 4 ||
      indexFile.write(&em, 2) != 2 || indexFile.write(&w, 2) != 2 || indexFile.write(&h, 2) != 2 ||
      indexFile.write(&modeWord, 2) != 2 || indexFile.write(&count, 4) != 4) {
    error = "index write";
    return false;
  }
  LOG_INF("TS", "index build");
  // Byte 0 is the IRA6 header. The first page replays from the first atom.
  const ts::WritingMode bookMode = layoutOpt.mode;
  if (!appendPage(4, static_cast<uint8_t>(bookMode), 0)) {
    return false;
  }
  atoms.seek(4);
  layouter.begin(layoutOpt);
  ts::Atom atom{};
  bool pendingStart = false;
  ts::WritingMode cur = bookMode;
  while (true) {
    const uint32_t atomPos = atoms.position();
    if (!atoms.next(atom)) {
      break;
    }
    const bool isMode = atom.kind == ts::AtomKind::Mode;
    const ts::WritingMode nextMode =
        isMode ? (atom.cp == 1 ? ts::WritingMode::HorizontalTb : ts::WritingMode::VerticalRl) : cur;
    const bool complete = layouter.feed(atom, atomPos);
    if (pendingStart && layouter.currentGlyphCount() > 0) {
      if (!appendPage(layouter.currentPagePos(), static_cast<uint8_t>(cur), 0)) {
        return false;
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
          return false;
        }
      } else if (!setLastPicture(taken)) {
        return false;
      }
      pendingStart = true;
    } else if (deferred != 0) {
      if (!appendPage(atomPos, static_cast<uint8_t>(cur), deferred)) {
        return false;
      }
      pendingStart = true;
    } else if (layouter.currentGlyphCount() > 0) {
      if (!appendPage(layouter.currentPagePos(), static_cast<uint8_t>(cur), 0)) {
        return false;
      }
      pendingStart = false;
    } else {
      pendingStart = true;
    }
  }
  if (!finishIndex()) {
    return false;
  }
  LOG_INF("TS", "Built index %u pages", nPages);
  return true;
}

bool TypesetBook::layoutPage(const uint32_t pageIndex) {
  if (pageIndex >= nPages) {
    error = "page range";
    return false;
  }
  if (loadedPage == pageIndex && (loadedCount > 0 || loadedPicture != 0)) {
    return true;
  }
  uint32_t atomOff = 0;
  uint8_t pageMode = static_cast<uint8_t>(layoutOpt.mode);
  uint16_t picture = 0;
  if (!readPage(pageIndex, atomOff, pageMode, picture)) {
    error = "page range";
    return false;
  }
  if (picture != 0) {
    loadedCount = 0;
    loadedPicture = picture;
    loadedPage = pageIndex;
    return true;
  }
  loadedPicture = 0;
  const unsigned long tLay = millis();
  atoms.seek(atomOff);
  ts::LayoutOptions pageOpt = layoutOpt;
  pageOpt.mode = static_cast<ts::WritingMode>(pageMode);
  layouter.begin(pageOpt);
  ts::Atom atom{};
  bool complete = false;
  while (true) {
    const uint32_t atomPos = atoms.position();
    if (!atoms.next(atom)) {
      break;
    }
    if (layouter.feed(atom, atomPos)) {
      complete = true;
      break;
    }
  }
  if (!complete) {
    layouter.finish();
  }
  loadedCount = layouter.pageGlyphCount();
  if (loadedCount > ts::PageLayouter::kMaxGlyphs) {
    loadedCount = ts::PageLayouter::kMaxGlyphs;
  }
  memcpy(loadedGlyphs, layouter.page(), sizeof(ts::GlyphRun) * loadedCount);
  loadedPage = pageIndex;
  const unsigned long layMs = millis() - tLay;

  const unsigned long tWarm = millis();
  uint16_t ids[ts::PageLayouter::kMaxGlyphs];
  uint16_t nIds = 0;
  for (uint16_t i = 0; i < loadedCount; ++i) {
    const uint16_t id = font.glyphId(loadedGlyphs[i].cp);
    if (id != 0xFFFF && nIds < ts::PageLayouter::kMaxGlyphs) {
      ids[nIds++] = id;
    }
    for (uint8_t r = 0; r < loadedGlyphs[i].rubyCount && nIds < ts::PageLayouter::kMaxGlyphs; ++r) {
      const uint16_t rid = font.glyphId(loadedGlyphs[i].ruby[r]);
      if (rid != 0xFFFF) {
        ids[nIds++] = rid;
      }
    }
  }
  font.prewarm(ids, nIds, false);
  font.prewarm(ids, nIds, true);
  LOG_INF("TS", "time layout %lums prewarm %lums glyphs=%u ids=%u page=%lu", layMs, millis() - tWarm, loadedCount, nIds,
          static_cast<unsigned long>(pageIndex + 1));
  return true;
}

namespace {

uint32_t fallbackCp(const uint32_t cp) {
  if (cp >= 0x2460 && cp <= 0x2468) {
    return static_cast<uint32_t>('1' + (cp - 0x2460));  // ①–⑨
  }
  if (cp == 0x2469) {
    return static_cast<uint32_t>('0');  // ⑩
  }
  return cp;
}

// Picture sidecar only. Glyphs deliberately do not use this map: XgfFont
// leaves its gray planes unmarked so the UC8253 nudge cannot eat the stem.
// 0 paper, 1 light, 2 dark, 3 black. Light marks the new plane only. Dark
// marks both. Black and paper stay on the B/W frame.
bool planeKeep(const uint8_t v, const XgfFont::Plane plane) {
  if (plane == XgfFont::Plane::Ink) {
    return v >= 1;
  }
  if (plane == XgfFont::Plane::Lsb) {
    return v == 2;
  }
  return v == 1 || v == 2;
}

// Logical portrait pixels, same 90° clockwise map as Gfx::drawPixel.
void blitPicture(Gfx& gfx, const uint8_t* bits, const int pageW, const int pageH, const XgfFont::Plane plane) {
  uint8_t* fb = gfx.frameBuffer();
  if (!fb || !bits || pageW <= 0 || pageH <= 0 || (pageW % 4) != 0) {
    return;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  const int rowBytes = pageW / 4;
  for (int y = 0; y < pageH; ++y) {
    const int phyX = y;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const bool black = plane == XgfFont::Plane::Ink;
    const uint8_t bit = static_cast<uint8_t>(1u << (7 - (phyX & 7)));
    const int col = phyX >> 3;
    const uint8_t* row = bits + static_cast<size_t>(y) * static_cast<size_t>(rowBytes);
    for (int x = 0; x < pageW; ++x) {
      const uint8_t v = static_cast<uint8_t>((row[x >> 2] >> (6 - 2 * (x & 3))) & 3);
      if (!planeKeep(v, plane)) {
        continue;
      }
      const int phyY = ph - 1 - x;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      uint8_t* cell = &fb[static_cast<size_t>(phyY) * stride + static_cast<size_t>(col)];
      if (black) {
        *cell &= static_cast<uint8_t>(~bit);
      } else {
        *cell |= bit;
      }
    }
  }
}

// Black pixels in a logical square. Bit 0 is black, same map as Gfx::drawPixel.
uint16_t cellBlack(const Gfx& gfx, const int x, const int y, const int size) {
  const uint8_t* fb = gfx.frameBuffer();
  if (!fb || size <= 0) {
    return 0;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  uint16_t n = 0;
  for (int ly = y; ly < y + size; ++ly) {
    const int phyX = ly;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (7 - (phyX & 7)));
    const int col = phyX >> 3;
    for (int lx = x; lx < x + size; ++lx) {
      const int phyY = ph - 1 - lx;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      if ((fb[static_cast<size_t>(phyY) * static_cast<size_t>(stride) + static_cast<size_t>(col)] & bit) == 0) {
        ++n;
      }
    }
  }
  return n;
}

int writeUtf8(const uint32_t cp, char* out) {
  if (cp < 0x21 || cp == 0x7F) {
    out[0] = '_';
    return 1;
  }
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = '?';
  return 1;
}

}  // namespace

void TypesetBook::paint(Gfx& gfx, const XgfFont::Plane plane) {
  // Ink is black on white. Gray planes are white marks on black, which is
  // what the waveform reads as light and dark.
  gfx.clear(plane != XgfFont::Plane::Ink);
  if (loadedPicture != 0) {
    if (pictureBits) {
      blitPicture(gfx, pictureBits, layoutOpt.width, layoutOpt.height, plane);
    }
    return;
  }
  if (plane != XgfFont::Plane::Ink) {
    // Stems stay on the ink frame. Walking glyphs here would miss the cache
    // while a refresh owns SPI and store a 0xFF block for that character.
    return;
  }
  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();

  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    const bool vert = !ts::runRubyAbove(g);
    const int adv = ts::runAdvance(g);
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;

    if (tcyRun) {
      const int n = g.rubyCount;
      int size = em / n;
      if (size < 1) {
        size = 1;
      }
      const int y0 = g.y + (em - size) / 2;
      const int x0 = g.x + (em - size * n) / 2;
      for (int r = 0; r < n; ++r) {
        uint16_t id = font.glyphId(g.ruby[r]);
        if (id == 0xFFFF) {
          id = font.glyphId(fallbackCp(g.ruby[r]));
        }
        if (id != 0xFFFF) {
          font.blitBox(gfx, x0 + r * size, y0, id, size, plane);
        }
      }
    } else {
      int drawX = g.x;
      int drawY = g.y;
      if ((g.flags & ts::kRunHalfCell) != 0 && g.advance > 0 && g.advance < g.size) {
        const int shift = (static_cast<int>(g.size) - static_cast<int>(g.advance)) / 2;
        if (vert) {
          drawY -= shift;
        } else {
          drawX -= shift;
        }
      }
      uint16_t id = font.glyphId(g.cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(g.cp));
      }
      if (id != 0xFFFF) {
        font.blit(gfx, drawX, drawY, id, false, ts::runRotate90(g), plane);
      }
    }

    if (g.emphasis != 0 && em > 0) {
      const bool hasRuby = !ts::runTcy(g) && g.rubyCount > 0;
      int gap = em / (hasRuby ? 10 : 4);
      if (gap < 1) {
        gap = 1;
      }
      if (g.emphasis == 2) {
        int thick = em / 14;
        if (thick < 1) {
          thick = 1;
        }
        if (vert) {
          gfx.fillRect(g.x + em + gap, g.y, thick, adv, true);
        } else {
          gfx.fillRect(g.x, g.y - gap - thick, adv, thick, true);
        }
      } else {
        int rad = em / 10;
        if (rad < 1) {
          rad = 1;
        }
        const int cx = vert ? g.x + em + gap : g.x + adv / 2;
        const int cy = vert ? g.y + adv / 2 : g.y - gap;
        const int r2 = rad * rad;
        for (int dy = -rad; dy <= rad; ++dy) {
          for (int dx = -rad; dx <= rad; ++dx) {
            if (dx * dx + dy * dy <= r2) {
              gfx.drawPixel(cx + dx, cy + dy, true);
            }
          }
        }
      }
    }

    if (tcyRun || g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }

    const int lead = g.rubyLead;
    const int origin = (vert ? g.y : g.x) - lead;
    int lastEnd = (vert ? g.y : g.x) + adv;
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < loadedCount; ++j) {
      const ts::GlyphRun& o = loadedGlyphs[j];
      if (!ts::runSticky(o)) {
        break;
      }
      if (vert) {
        if (o.x != g.x) {
          break;
        }
      } else if (o.y != g.y) {
        break;
      }
      lastEnd = (vert ? o.y : o.x) + ts::runAdvance(o);
    }
    const int span = lastEnd - origin + lead;
    if (span <= 0) {
      continue;
    }
    const int clusterLo = origin;
    const int clusterHi = origin + span;
    auto inside = [&](const ts::GlyphRun& o) -> bool {
      if (vert) {
        return o.x == g.x && o.y >= clusterLo && o.y < clusterHi;
      }
      return o.y == g.y && o.x >= clusterLo && o.x < clusterHi;
    };
    auto blocks = [&](const int dir) -> bool {
      const ts::GlyphRun* best = nullptr;
      int bestAbs = 0;
      for (uint16_t j = 0; j < loadedCount; ++j) {
        const ts::GlyphRun& o = loadedGlyphs[j];
        if (inside(o)) {
          continue;
        }
        if (vert) {
          if (o.x != g.x) {
            continue;
          }
        } else if (o.y != g.y) {
          continue;
        }
        const int d = (vert ? o.y : o.x) - origin;
        if ((dir < 0 && d >= 0) || (dir > 0 && d <= 0)) {
          continue;
        }
        const int ad = d < 0 ? -d : d;
        if (!best || ad < bestAbs) {
          best = &o;
          bestAbs = ad;
        }
      }
      return best && best->rubyCount > 0 && bestAbs <= span;
    };
    const ts::RubyAlong along = ts::placeRubyAlong(static_cast<int16_t>(origin), static_cast<int16_t>(span),
                                                   static_cast<int16_t>(rubyEm), g.rubyCount, !blocks(-1), !blocks(1));
    const int beside = g.emphasis ? em * 28 / 100 : 0;
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      uint16_t rid = font.glyphId(g.ruby[r]);
      if (rid == 0xFFFF) {
        rid = font.glyphId(fallbackCp(g.ruby[r]));
      }
      if (rid == 0xFFFF) {
        continue;
      }
      const int at = along.start + r * along.pitch;
      const int rx = vert ? g.x + em + beside : at;
      const int ry = vert ? at : g.y - rubyEm - beside;
      font.blit(gfx, rx, ry, rid, true, false, plane);
    }
  }
}

void TypesetBook::logPaintedGlyphs(Gfx& gfx, const uint32_t pageIndex) const {
  const XgfFont::CacheStats st = font.cacheStats();
  if (loadedPicture != 0) {
    LOG_INF("TS", "gly p%lu picture=%u", static_cast<unsigned long>(pageIndex + 1), loadedPicture);
    return;
  }
  LOG_INF("TS", "gly p%lu runs=%u em=%u body=%u/%u ruby=%u/%u hit=%lu miss=%lu ff=%lu blk=%lu",
          static_cast<unsigned long>(pageIndex + 1), loadedCount, font.emPx(), st.bodyUsed, st.bodyCap, st.rubyUsed,
          st.rubyCap, static_cast<unsigned long>(st.hits), static_cast<unsigned long>(st.misses),
          static_cast<unsigned long>(st.rejected), static_cast<unsigned long>(st.blocked));

  // One token per codepoint: char+cp id hash sourceInk/drawnBlack flags.
  // B = cached bitmap is mostly ink. D = the cell is much blacker than the
  // bitmap. M = not in the cache, so the cell was left blank. r = ruby.
  static uint32_t seen[160];
  uint16_t nSeen = 0;
  char line[160];
  size_t used = 0;
  auto fresh = [&](const uint32_t key) -> bool {
    for (uint16_t i = 0; i < nSeen; ++i) {
      if (seen[i] == key) {
        return false;
      }
    }
    if (nSeen < 160) {
      seen[nSeen++] = key;
    }
    return true;
  };
  auto flush = [&]() {
    if (used == 0) {
      return;
    }
    line[used] = '\0';
    LOG_INF("TS", "gly %s", line);
    used = 0;
  };
  auto add = [&](const char* tok) {
    const size_t n = strlen(tok);
    if (used != 0 && used + 1 + n >= sizeof(line)) {
      flush();
    }
    if (used != 0 && used + 1 < sizeof(line)) {
      line[used++] = ' ';
    }
    if (n >= sizeof(line)) {
      return;
    }
    memcpy(line + used, tok, n);
    used += n;
  };
  auto token = [&](const uint32_t cp, const uint16_t id, const bool ruby, const int x, const int y, const int size,
                   const uint8_t flags, const bool measure) {
    char utf[4];
    const int un = writeUtf8(cp, utf);
    XgfFont::SlotProbe pr;
    const bool ok = id != 0xFFFF && font.probe(id, ruby, pr);
    const uint16_t dst = measure ? cellBlack(gfx, x, y, size) : 0;
    char mark[4] = {};
    size_t m = 0;
    if (!ok) {
      mark[m++] = 'M';
    } else if (pr.area != 0 && static_cast<uint32_t>(pr.ink) * 100u / pr.area >= 55u) {
      mark[m++] = 'B';
    }
    if (measure && ok && dst > static_cast<uint16_t>(pr.ink + pr.ink / 3 + 40)) {
      mark[m++] = 'D';
    }
    char tok[72];
    if (measure) {
      snprintf(tok, sizeof(tok), "%s%.*s+%04lX/%04X/%08lX/%u/%u/%02X%s", ruby ? "r" : "", un, utf,
               static_cast<unsigned long>(cp), id, static_cast<unsigned long>(ok ? pr.hash : 0), ok ? pr.ink : 0, dst,
               flags, mark);
    } else {
      snprintf(tok, sizeof(tok), "%s%.*s+%04lX/%04X/%08lX/%u/-/%02X%s", ruby ? "r" : "", un, utf,
               static_cast<unsigned long>(cp), id, static_cast<unsigned long>(ok ? pr.hash : 0), ok ? pr.ink : 0, flags,
               mark);
    }
    add(tok);
  };

  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();
  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    const bool vert = !ts::runRubyAbove(g);
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;
    if (tcyRun) {
      const int n = g.rubyCount;
      int size = em / n;
      if (size < 1) {
        size = 1;
      }
      const int y0 = g.y + (em - size) / 2;
      const int x0 = g.x + (em - size * n) / 2;
      for (int r = 0; r < n; ++r) {
        const uint32_t cp = g.ruby[r];
        if (!fresh(cp | 0x40000000u)) {
          continue;
        }
        uint16_t id = font.glyphId(cp);
        if (id == 0xFFFF) {
          id = font.glyphId(fallbackCp(cp));
        }
        token(cp, id, false, x0 + r * size, y0, size, g.flags, false);
      }
      continue;
    }

    int drawX = g.x;
    int drawY = g.y;
    if ((g.flags & ts::kRunHalfCell) != 0 && g.advance > 0 && g.advance < g.size) {
      const int shift = (static_cast<int>(g.size) - static_cast<int>(g.advance)) / 2;
      if (vert) {
        drawY -= shift;
      } else {
        drawX -= shift;
      }
    }
    if (fresh(g.cp)) {
      uint16_t id = font.glyphId(g.cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(g.cp));
      }
      token(g.cp, id, false, drawX, drawY, em, g.flags, true);
    }
    if (g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      const uint32_t cp = g.ruby[r];
      if (!fresh(cp | 0x80000000u)) {
        continue;
      }
      uint16_t id = font.glyphId(cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(cp));
      }
      token(cp, id, true, 0, 0, rubyEm, g.flags, false);
    }
  }
  flush();
}

bool TypesetBook::drawPage(Gfx& gfx, const uint32_t pageIndex, int& pagesUntilFullRefresh,
                          const int refreshFrequency) {
  if (!opened) {
    error = "closed";
    return false;
  }
  const unsigned long tDraw = millis();
  const XgfFont::CacheStats sd0 = font.cacheStats();
  const unsigned long tRebase = millis();
  flushPendingCleanup(gfx);
  const unsigned long rebaseMs = millis() - tRebase;
  const unsigned long tLay = millis();
  if (!layoutPage(pageIndex)) {
    return false;
  }
  const unsigned long layMs = millis() - tLay;
  // SD and the panel share SPI. Read the page before the grayscale waveform starts.
  const unsigned long tPic = millis();
  pictureBits = nullptr;
  if (loadedPicture != 0) {
    const uint16_t pw = static_cast<uint16_t>(layoutOpt.width);
    const uint16_t ph = static_cast<uint16_t>(layoutOpt.height);
    const size_t nbytes = static_cast<size_t>(pw / 4) * ph;
    uint8_t* dst = ScratchHeap::data();
    if (dst && ScratchHeap::size() >= nbytes && ts::pictureRead(picturePath, loadedPicture, pw, ph, dst, nbytes)) {
      pictureBits = dst;
    } else {
      LOG_ERR("TS", "picture %u missing", loadedPicture);
    }
  }
  const unsigned long picMs = millis() - tPic;
  const unsigned long tInk = millis();
  paint(gfx, XgfFont::Plane::Ink);
  const unsigned long inkMs = millis() - tInk;
  // After the displayed ink frame, before the refresh takes the SPI bus.
  const unsigned long tGly = millis();
  logPaintedGlyphs(gfx, pageIndex);
  const unsigned long glyMs = millis() - tGly;

  const unsigned long tBase = millis();
  if (pagesUntilFullRefresh <= 1) {
    if (gfx.combinesGrayscaleBase()) {
      gfx.startGrayscaleBase(HalDisplay::HALF_REFRESH);
    } else {
      gfx.present(HalDisplay::HALF_REFRESH);
      gfx.preconditionGrayscale();
    }
    pagesUntilFullRefresh = refreshFrequency;
  } else {
    gfx.startGrayscaleBase(HalDisplay::FAST_REFRESH);
    --pagesUntilFullRefresh;
  }
  const unsigned long baseFireMs = millis() - tBase;

  // The fast base refresh still owns SPI. Glyph gray is unmarked, and a miss
  // must not read the card (that 0xFF slot is the repeating block).
  font.setSdReads(false);
  const unsigned long tLsb = millis();
  paint(gfx, XgfFont::Plane::Lsb);
  const unsigned long lsbMs = millis() - tLsb;

  const unsigned long tBaseWait = millis();
  gfx.finishGrayscaleBase();
  const unsigned long baseWaitMs = millis() - tBaseWait;
  font.setSdReads(true);

  const unsigned long tLsbCopy = millis();
  gfx.copyGrayscaleLsbBuffers();
  const unsigned long lsbCopyMs = millis() - tLsbCopy;
  paint(gfx, XgfFont::Plane::Msb);
  const unsigned long tMsbCopy = millis();
  gfx.copyGrayscaleMsbBuffers();
  const unsigned long msbCopyMs = millis() - tMsbCopy;

  font.setSdReads(false);
  gfx.startGrayBuffer();
  const unsigned long tWait = millis();
  gfx.finishGrayBuffer();
  const unsigned long waitMs = millis() - tWait;
  font.setSdReads(true);

  // Cleanup image for the next page's differential. SPI is idle, so a miss
  // is a real glyph. Doing this before finishGrayBuffer cached the idle
  // 0xFF read, and that character stayed a block on every later page.
  const unsigned long tClean = millis();
  paint(gfx, XgfFont::Plane::Ink);
  const unsigned long cleanMs = millis() - tClean;
  const XgfFont::CacheStats sd1 = font.cacheStats();
  // Milliseconds except sd reads/bytes and the hit/miss counts. spi is the
  // base send, then the LSB copy, then the MSB copy. wave is the two BUSY waits.
  LOG_INF("TS",
          "perf p%lu lay %lu pic %lu ink %lu lsb %lu clean %lu gly %lu rebase %lu spi %lu/%lu/%lu wave %lu+%lu sd %lu/%lu/%lu "
          "hit+%lu miss+%lu body %u/%u total %lu",
          static_cast<unsigned long>(pageIndex + 1), layMs, picMs, inkMs, lsbMs, cleanMs, glyMs, rebaseMs, baseFireMs,
          lsbCopyMs, msbCopyMs, baseWaitMs, waitMs, static_cast<unsigned long>(sd1.sdReads - sd0.sdReads),
          static_cast<unsigned long>(sd1.sdBytes - sd0.sdBytes),
          static_cast<unsigned long>((sd1.sdUs - sd0.sdUs) / 1000u), static_cast<unsigned long>(sd1.hits - sd0.hits),
          static_cast<unsigned long>(sd1.misses - sd0.misses), sd1.bodyUsed, sd1.bodyCap, millis() - tDraw);
  cleanupPending = true;
  error = "";
  return true;
}

void TypesetBook::prefetchForward(const uint32_t fromPageIndex) {
  if (!opened || fromPageIndex + 1 >= nPages) {
    return;
  }
  const unsigned long t0 = millis();
  const XgfFont::CacheStats sd0 = font.cacheStats();
  (void)layoutPage(fromPageIndex + 1);
  const XgfFont::CacheStats sd1 = font.cacheStats();
  LOG_INF("TS", "perf prefetch p%lu %lums sd %lu/%lu/%lu miss+%lu", static_cast<unsigned long>(fromPageIndex + 2),
          millis() - t0, static_cast<unsigned long>(sd1.sdReads - sd0.sdReads),
          static_cast<unsigned long>(sd1.sdBytes - sd0.sdBytes),
          static_cast<unsigned long>((sd1.sdUs - sd0.sdUs) / 1000u),
          static_cast<unsigned long>(sd1.misses - sd0.misses));
}

void TypesetBook::flushPendingCleanup(Gfx& gfx) {
  if (!cleanupPending) {
    return;
  }
  gfx.cleanupGrayscaleBuffers();
  cleanupPending = false;
}
