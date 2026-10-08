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

  bool open(const char* path, ts::EpubBook::ProgressFn progress = nullptr, void* progressCtx = nullptr,
            const char* fontPath = nullptr);
  void close();
  bool isOpen() const { return opened; }

  uint16_t pageCount() const { return static_cast<uint16_t>(pageOffsets.size()); }
  const char* title() const { return bookTitle; }
  const char* path() const { return filepath; }
  const char* lastError() const { return error; }

  const std::vector<ts::ChapterInfo>& getChapters() const { return chapters; }
  XgfFont* cjkFont() { return font.loaded() ? &font : nullptr; }

  bool drawPage(Gfx& gfx, uint32_t pageIndex, int& pagesUntilFullRefresh, int refreshFrequency);
  void prefetchForward(uint32_t fromPageIndex);
  void flushPendingCleanup(Gfx& gfx);

  static bool hasBookExt(const char* path);

 private:
  char filepath[256]{};
  char bookTitle[128]{};
  char atomPath[64]{};
  const char* error = "not open";
  XgfFont font;
  ts::AtomReader atoms;
  ts::PageLayouter layouter;
  ts::LayoutOptions layoutOpt{};
  std::vector<uint32_t> pageOffsets;
  std::vector<ts::ChapterInfo> chapters;
  struct ChapterMark {
    uint32_t atomOff = 0;
    char name[80]{};
  };
  std::vector<ChapterMark> chapterMarks;
  uint32_t sourceSize = 0;
  uint32_t bookSrcSize = 0;
  bool opened = false;
  bool cleanupPending = false;
  uint32_t loadedPage = 0xFFFFFFFFu;
  ts::GlyphRun loadedGlyphs[ts::PageLayouter::kMaxGlyphs]{};
  uint16_t loadedCount = 0;

  bool loadFont(const char* fontPath);
  bool ingestTxt();
  bool ingestEpub(ts::EpubBook::ProgressFn progress, void* progressCtx);
  bool buildIndex();
  bool loadIndex();
  bool saveIndex() const;
  bool atomCacheFresh() const;
  bool layoutPage(uint32_t pageIndex);
  void paint(Gfx& gfx, XgfFont::Plane plane);
  void indexPath(char* out, size_t outSize) const;
  void chapterPath(char* out, size_t outSize) const;
  bool saveChapterSidecar() const;
  bool loadChapterSidecar();
  void applyChapters();
  uint16_t pageForAtom(uint32_t atomOff) const;
};
