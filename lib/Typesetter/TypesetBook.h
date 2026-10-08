#pragma once

#include <XgfFont.h>

#include <cstdint>
#include <vector>

#include "AtomFile.h"
#include "ChapterInfo.h"
#include "Epub.h"
#include "Layout.h"
#include "TextIr.h"

class Gfx;

// Japanese reader: EPUB (lazahata IR) or UTF-8 text → em-grid + XGF2 blit.
class TypesetBook {
 public:
  TypesetBook() = default;
  ~TypesetBook() { close(); }

  // `resumePage` / `resumeAtom` are the saved reading position. A cold open
  // ingests and lays out only the opening chapter; a resume lays out the chapter
  // that contains `resumeAtom` and leaves the rest for extendAhead().
  bool open(const char* path, ts::EpubBook::ProgressFn progress = nullptr, void* progressCtx = nullptr,
            const char* fontPath = nullptr, uint32_t resumePage = 0, uint32_t resumeAtom = 0);
  void close();
  bool isOpen() const { return opened; }

  // Exact once the book is fully indexed. Until then, an estimate from the
  // atoms and spine items already seen, never below the pages that exist.
  uint16_t pageCount() const;
  uint16_t indexedSpan() const { return nPages; }
  uint16_t pagesBefore() const { return frontPages; }
  bool buildFinished() const { return ingestDone && coversEnd && originAtom <= 4; }
  // Global page open() can paint. Meaningful when openedPartial() is true.
  uint32_t showAt() const { return showPage; }
  bool openedPartial() const { return partialOpen; }
  uint32_t atomForPage(uint32_t globalPage) const;
  uint32_t globalForAtom(uint32_t atom) const;
  // Lay out a few pages past `globalPage`, ingesting the next spine item when
  // the atom prefix runs out. One idle tick.
  bool extendAhead(uint32_t globalPage, uint16_t ahead);
  bool ensureGlobal(uint32_t globalPage, ts::EpubBook::ProgressFn progress = nullptr, void* progressCtx = nullptr);
  bool ensureChapter(uint16_t chapterIndex, uint32_t& globalPage, ts::EpubBook::ProgressFn progress = nullptr,
                     void* progressCtx = nullptr);
  const char* title() const { return bookTitle; }
  const char* path() const { return filepath; }
  const char* lastError() const { return error; }

  // Recomputes page ranges for chapters whose atoms are already indexed.
  const std::vector<ts::ChapterInfo>& getChapters();
  XgfFont* cjkFont() { return font.loaded() ? &font : nullptr; }

  bool drawPage(Gfx& gfx, uint32_t pageIndex, int& pagesUntilFullRefresh, int refreshFrequency);
  void prefetchForward(uint32_t fromPageIndex);
  void flushPendingCleanup(Gfx& gfx);

  static bool hasBookExt(const char* path);

 private:
  // TID7 is a finished index: 24-byte header, then 7-byte records.
  // TID8 keeps that header and adds a 24-byte tail so a chapter window can
  // resume. Records then start at byte 48. Promotion back to TID7 only
  // happens for a complete prefix that starts at the first atom.
  static constexpr uint32_t kIndexMagic = 0x37444954u;
  static constexpr uint32_t kIndexPartial = 0x38444954u;
  static constexpr uint32_t kIndexPayload = 24;
  static constexpr uint32_t kPartialPayload = 48;
  static constexpr uint32_t kFlagIngest = 1u;
  static constexpr uint32_t kFlagCovers = 2u;
  static constexpr uint32_t kFlagPending = 4u;
  static constexpr uint32_t kFlagFrozen = 8u;
  static constexpr uint32_t kCursorMagic = 0x314E5053u;
  static constexpr uint16_t kMaxPages = 65535;
  static constexpr const char* kWorkIndex = "/.crossjp/work.idx";

  char filepath[256]{};
  char bookTitle[128]{};
  char atomPath[64]{};
  char picturePath[64]{};
  const char* error = "not open";
  XgfFont font;
  ts::AtomReader atoms;
  ts::PageLayouter layouter;
  ts::LayoutOptions layoutOpt{};
  // Page records stay on the index file. A seven-volume book is tens of thousands
  // of pages, and holding the three tables in RAM aborts in operator new.
  mutable HalFile indexFile;
  uint16_t nPages = 0;
  uint8_t indexBuf[504]{};
  uint16_t indexBufN = 0;
  std::vector<ts::ChapterInfo> chapters;
  struct ChapterMark {
    uint32_t atomOff = 0xFFFFFFFFu;
    uint16_t spine = 0xFFFFu;
    char name[80]{};
  };
  std::vector<ChapterMark> chapterMarks;
  uint32_t sourceSize = 0;
  uint32_t bookSrcSize = 0;
  bool opened = false;
  bool atomsLive = false;
  bool ingestDone = false;
  bool coversEnd = false;
  bool pendingPage = false;
  bool frontFrozen = false;
  bool extendFailed = false;
  bool partialOpen = false;
  bool cursorOk = false;
  uint16_t indexBase = 24;
  uint16_t nextSpine = 0;
  uint16_t spineCount = 0;
  uint16_t originSpine = 0;
  uint16_t frontPages = 0;
  // Writing mode to use when the next page record has not been started.
  uint16_t runMode = 0;
  uint32_t originAtom = 4;
  uint32_t indexedAtom = 4;
  uint32_t atomBytes = 0;
  uint32_t showPage = 0;
  bool cleanupPending = false;
  uint32_t loadedPage = 0xFFFFFFFFu;
  ts::GlyphRun loadedGlyphs[ts::PageLayouter::kMaxGlyphs]{};
  uint16_t loadedCount = 0;
  uint16_t loadedPicture = 0;
  uint8_t* pictureBits = nullptr;  // borrowed ScratchHeap page, valid only while painting

  bool loadFont(const char* fontPath);
  enum class IndexStep { Ok, NeedAtoms, Done, Error };

  bool ingestTxt();
  bool ingestEpub(ts::EpubBook::ProgressFn progress, void* progressCtx);
  bool ingestSlice(uint16_t maxSpines, ts::EpubBook::ProgressFn progress, void* progressCtx);
  bool loadIndex();
  bool beginRange(uint32_t origin, uint16_t spine);
  IndexStep indexMore(uint16_t budget, uint32_t stopAtom);
  bool indexUntil(uint32_t target, ts::EpubBook::ProgressFn progress, void* progressCtx);
  bool prependOne();
  bool suspendIndex();
  bool promoteIndex();
  bool writePartialHeader();
  bool pageCovers(uint32_t atom) const;
  void freezeFront();
  void mergeToc(const ts::EpubBook& epub);
  bool loadCursor();
  bool saveCursor() const;
  void cursorPath(char* out, size_t outSize) const;
  bool peekAtom(uint32_t globalPage, uint32_t& atom) const;
  void dropPartialCache();
  bool reopenAtoms();
  bool reopenIndex();
  uint32_t localOf(uint32_t globalPage) const;
  bool finishIndex();
  bool flushIndex();
  bool appendPage(uint32_t off, uint8_t mode, uint16_t pic);
  bool setLastPicture(uint16_t pic);
  bool readPage(uint32_t index, uint32_t& off, uint8_t& mode, uint16_t& pic) const;
  bool atomCacheFresh() const;
  bool layoutPage(uint32_t pageIndex);
  void paint(Gfx& gfx, XgfFont::Plane plane);
  void logPaintedGlyphs(Gfx& gfx, uint32_t pageIndex) const;
  void indexPath(char* out, size_t outSize) const;
  void chapterPath(char* out, size_t outSize) const;
  bool saveChapterSidecar() const;
  bool loadChapterSidecar();
  void applyChapters();
};
