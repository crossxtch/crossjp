#include "TypesetBook.h"

#include <BoardConfig.h>
#include <Gfx.h>
#include <Logging.h>
#include <ScratchHeap.h>

#include <cstdio>
#include <cstring>

namespace {

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

  snprintf(atomPath, sizeof(atomPath), "/.crossjp/a_%08lx.bin", static_cast<unsigned long>(pathHash(filepath)));
  Storage.ensureDirectoryExists("/.crossjp");

  bookSrcSize = fileSizeOf(filepath);
  sourceSize = fileSizeOf(atomPath);
  const bool epub = endsWithI(filepath, ".epub");
  bool fresh = atomCacheFresh();
  if (epub && fresh && !loadChapterSidecar()) {
    fresh = false;
    LOG_INF("TS", "Reingest for chapter TOC");
  }
  if (fresh) {
    LOG_INF("TS", "time ingest 0ms skip (atom cache)");
  } else {
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
  pageOffsets.clear();
  chapters.clear();
  chapterMarks.clear();
  sourceSize = 0;
  bookSrcSize = 0;
  loadedPage = 0xFFFFFFFFu;
  loadedCount = 0;
  cleanupPending = false;
  ScratchHeap::reserve(BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
}

bool TypesetBook::ingestTxt() {
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
      error = "atom write";
      w.close();
      return false;
    }
  }
  w.close();
  return true;
}

bool TypesetBook::ingestEpub(ts::EpubBook::ProgressFn progress, void* progressCtx) {
  font.releaseMaps();
  ts::EpubBook epub;
  const bool ok = epub.open(filepath, atomPath, progress, progressCtx);
  if (!font.restoreMaps()) {
    error = "font maps";
    return false;
  }
  if (!ok) {
    error = epub.lastError()[0] ? epub.lastError() : "epub";
    return false;
  }
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
  if (f.read(&magic, 4) != 4 || magic != 0x33544843u || f.read(&n, 2) != 2 || n > 192) {
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

uint16_t TypesetBook::pageForAtom(const uint32_t atomOff) const {
  if (pageOffsets.empty()) {
    return 0;
  }
  uint16_t lo = 0;
  uint16_t hi = static_cast<uint16_t>(pageOffsets.size());
  while (static_cast<uint16_t>(lo + 1) < hi) {
    const uint16_t mid = static_cast<uint16_t>((lo + hi) / 2);
    if (pageOffsets[mid] <= atomOff) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return lo;
}

void TypesetBook::applyChapters() {
  chapters.clear();
  if (chapterMarks.empty() || pageOffsets.empty()) {
    return;
  }
  chapters.reserve(chapterMarks.size());
  for (const auto& m : chapterMarks) {
    ts::ChapterInfo ch;
    ch.name = m.name;
    ch.startPage = pageForAtom(m.atomOff);
    ch.endPage = ch.startPage;
    chapters.push_back(std::move(ch));
  }
  const uint16_t last = pageCount() > 0 ? static_cast<uint16_t>(pageCount() - 1) : 0;
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
  // Broken ingest after cache-clear wrote only page-breaks (~1 byte each).
  if (endsWithI(filepath, ".epub") && sourceSize < 2048) {
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
  if (magic == 0x34444954u /* TID4 */) {
    if (idx.read(&atomSz, 4) != 4 || atomSz != sourceSize) {
      return false;
    }
    if (idx.read(&srcSz, 4) != 4 || (bookSrcSize != 0 && srcSz != bookSrcSize)) {
      return false;  // EPUB replaced
    }
  }
  return true;
}

bool TypesetBook::loadIndex() {
  char p[64];
  indexPath(p, sizeof(p));
  HalFile idx;
  if (!Storage.openFileForRead("TS", p, idx)) {
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
  if (idx.read(&magic, 4) != 4 || magic != 0x34444954u /* TID4 */ || idx.read(&atomSz, 4) != 4 ||
      atomSz != sourceSize || idx.read(&srcSz, 4) != 4 || srcSz != bookSrcSize || idx.read(&em, 2) != 2 ||
      em != layoutOpt.em || idx.read(&w, 2) != 2 || w != static_cast<uint16_t>(layoutOpt.width) ||
      idx.read(&h, 2) != 2 || h != static_cast<uint16_t>(layoutOpt.height) || idx.read(&mode, 2) != 2 ||
      idx.read(&count, 4) != 4 || count == 0 || count > 65535) {
    return false;
  }
  layoutOpt.mode = static_cast<ts::WritingMode>(mode);
  pageOffsets.resize(count);
  const size_t bytes = count * 4;
  if (idx.read(pageOffsets.data(), bytes) != static_cast<int>(bytes)) {
    pageOffsets.clear();
    return false;
  }
  LOG_INF("TS", "Index %s (%u pages)", p, static_cast<unsigned>(count));
  return true;
}

bool TypesetBook::saveIndex() const {
  char p[64];
  indexPath(p, sizeof(p));
  Storage.ensureDirectoryExists("/.crossjp");
  HalFile idx;
  if (!Storage.openFileForWrite("TS", p, idx)) {
    return false;
  }
  const uint32_t magic = 0x34444954u;
  const uint32_t atomSz = sourceSize;
  const uint32_t srcSz = bookSrcSize;
  const uint16_t em = layoutOpt.em;
  const uint16_t w = static_cast<uint16_t>(layoutOpt.width);
  const uint16_t h = static_cast<uint16_t>(layoutOpt.height);
  const uint16_t mode = static_cast<uint16_t>(layoutOpt.mode);
  const uint32_t count = static_cast<uint32_t>(pageOffsets.size());
  idx.write(&magic, 4);
  idx.write(&atomSz, 4);
  idx.write(&srcSz, 4);
  idx.write(&em, 2);
  idx.write(&w, 2);
  idx.write(&h, 2);
  idx.write(&mode, 2);
  idx.write(&count, 4);
  idx.write(pageOffsets.data(), count * 4);
  return true;
}

bool TypesetBook::buildIndex() {
  pageOffsets.clear();
  pageOffsets.push_back(0);
  atoms.seek(0);
  layouter.begin(layoutOpt);
  ts::Atom atom{};
  bool pendingStart = false;
  while (true) {
    const uint32_t atomPos = atoms.position();
    if (!atoms.next(atom)) {
      break;
    }
    const bool complete = layouter.feed(atom, atomPos);
    if (pendingStart && layouter.currentGlyphCount() > 0) {
      if (pageOffsets.size() >= 65535) {
        error = "too many pages";
        return false;
      }
      pageOffsets.push_back(layouter.currentPagePos());
      pendingStart = false;
    }
    if (complete) {
      layouter.clearPage();
      if (layouter.currentGlyphCount() > 0) {
        if (pageOffsets.size() >= 65535) {
          error = "too many pages";
          return false;
        }
        pageOffsets.push_back(layouter.currentPagePos());
      } else {
        pendingStart = true;
      }
    }
  }
  LOG_INF("TS", "Built index %u pages", static_cast<unsigned>(pageOffsets.size()));
  saveIndex();
  return true;
}

bool TypesetBook::layoutPage(const uint32_t pageIndex) {
  if (pageIndex >= pageOffsets.size()) {
    error = "page range";
    return false;
  }
  if (loadedPage == pageIndex && loadedCount > 0) {
    return true;
  }
  const unsigned long tLay = millis();
  atoms.seek(pageOffsets[pageIndex]);
  layouter.begin(layoutOpt);
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

}  // namespace

void TypesetBook::paint(Gfx& gfx, const XgfFont::Plane plane) {
  gfx.clear(false);
  const uint8_t em = font.emPx();
  const uint8_t rubyEm = font.rubyEmPx();

  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    uint16_t id = font.glyphId(g.cp);
    if (id == 0xFFFF) {
      id = font.glyphId(fallbackCp(g.cp));
    }
    if (id != 0xFFFF) {
      font.blit(gfx, g.x, g.y, id, false, ts::runRotate90(g), plane);
    }
    if (g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }
    const bool vert = !ts::runRubyAbove(g);
    const int16_t origin = vert ? g.y : g.x;
    const int16_t cell = g.size > 0 ? static_cast<int16_t>(g.size) : static_cast<int16_t>(em);
    auto blocks = [&](const int dir) -> bool {
      const ts::GlyphRun* best = nullptr;
      int bestAbs = 0;
      for (uint16_t j = 0; j < loadedCount; ++j) {
        if (j == i) {
          continue;
        }
        const ts::GlyphRun& o = loadedGlyphs[j];
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
      return best && best->rubyCount > 0 && bestAbs <= cell;
    };
    const ts::RubyAlong along =
        ts::placeRubyAlong(origin, cell, static_cast<int16_t>(rubyEm), g.rubyCount, !blocks(-1), !blocks(1));
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      const uint16_t rid = font.glyphId(g.ruby[r]);
      if (rid == 0xFFFF) {
        continue;
      }
      const int at = along.start + r * along.pitch;
      const int rx = vert ? g.x + em : at;
      const int ry = vert ? at : g.y - rubyEm;
      font.blit(gfx, rx, ry, rid, true, false, plane);
    }
  }
}

bool TypesetBook::drawPage(Gfx& gfx, const uint32_t pageIndex, int& pagesUntilFullRefresh,
                          const int refreshFrequency) {
  if (!opened) {
    error = "closed";
    return false;
  }
  flushPendingCleanup(gfx);
  const unsigned long tDraw = millis();
  if (!layoutPage(pageIndex)) {
    return false;
  }
  const unsigned long tInk = millis();
  paint(gfx, XgfFont::Plane::Ink);
  const unsigned long inkMs = millis() - tInk;

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

  // CPU-only: fb is free (controller already has the ink plane). No SD/SPI.
  const unsigned long tLsb = millis();
  paint(gfx, XgfFont::Plane::Lsb);
  const unsigned long lsbMs = millis() - tLsb;

  const unsigned long tBaseWait = millis();
  gfx.finishGrayscaleBase();
  const unsigned long baseWaitMs = millis() - tBaseWait;

  const unsigned long tGray = millis();
  gfx.copyGrayscaleLsbBuffers();
  paint(gfx, XgfFont::Plane::Msb);
  gfx.copyGrayscaleMsbBuffers();
  const unsigned long grayPaintMs = millis() - tGray;

  gfx.startGrayBuffer();
  paint(gfx, XgfFont::Plane::Ink);
  const unsigned long tWait = millis();
  gfx.finishGrayBuffer();
  const unsigned long waitMs = millis() - tWait;
  LOG_INF("TS",
          "time draw p%lu ink %lums baseFire %lums lsb %lums baseWait %lums gray %lums panel %lums total %lums",
          static_cast<unsigned long>(pageIndex + 1), inkMs, baseFireMs, lsbMs, baseWaitMs, grayPaintMs, waitMs,
          millis() - tDraw);
  cleanupPending = true;
  error = "";
  return true;
}

void TypesetBook::prefetchForward(const uint32_t fromPageIndex) {
  if (!opened || fromPageIndex + 1 >= pageOffsets.size()) {
    return;
  }
  (void)layoutPage(fromPageIndex + 1);
}

void TypesetBook::flushPendingCleanup(Gfx& gfx) {
  if (!cleanupPending) {
    return;
  }
  gfx.cleanupGrayscaleBuffers();
  cleanupPending = false;
}
