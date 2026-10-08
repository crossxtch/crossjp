#pragma once

#include "AtomFile.h"
#include "HtmlIR.h"
#include "PicturePage.h"
#include "Zip.h"

#include <cstdint>

namespace ts {

class HtmlSax;

class EpubBook {
 public:
  // Spine slots kept for one ingest. A combined series can list several hundred
  // files. Each slot stays small so a 32KB deflate window still fits beside the
  // zip table (the previous 212-byte slot left only a 26KB hole).
  static constexpr uint16_t kMaxSpine = 512;

  EpubBook() = default;
  ~EpubBook() { close(); }
  EpubBook(const EpubBook&) = delete;
  EpubBook& operator=(const EpubBook&) = delete;

  using ProgressFn = void (*)(void* ctx, uint16_t done, uint16_t total);

  // pageW/pageH are the logical portrait. picturePath receives PIC1 pages. Either may be empty.
  // `maxSpines` of 0xFFFF ingests the whole spine (the historical open). A smaller
  // count ingests [spineBegin, spineBegin + maxSpines). `appendAtoms` keeps the
  // IRA6 prefix and truncates it back to `atomKeep` before writing the next slice.
  bool open(const char* epubPath, const char* atomPath, ProgressFn progress = nullptr, void* progressCtx = nullptr,
            uint16_t pageW = 0, uint16_t pageH = 0, const char* picturePath = nullptr, uint16_t spineBegin = 0,
            uint16_t maxSpines = 0xFFFF, bool appendAtoms = false, uint32_t atomKeep = 0);
  void close();

  const char* title() const { return bookTitle; }
  const char* author() const { return bookAuthor; }
  WritingMode bookMode() const { return mode; }
  uint16_t spineCount() const { return nSpine; }
  // One past the last spine this open() walked. Equals spineCount() when the file is finished.
  uint16_t ingestedEnd() const { return sliceEnd; }
  const char* lastError() const { return error; }

  struct Spine {
    static constexpr uint8_t kOther = 0;
    static constexpr uint8_t kHtml = 1;
    static constexpr uint8_t kImage = 2;
    static constexpr uint8_t kSvg = 3;
    // Index into the open zip. 0xFFFF until parse resolves the href.
    uint16_t zipIndex = 0xFFFFu;
    uint16_t chCount = 0;
    uint32_t atomOff = 0;
    uint8_t kind = kOther;
    uint8_t compact = 0;
    uint8_t hasMode = 0;
    uint8_t done = 0;
    WritingMode mode = WritingMode::VerticalRl;
    char title[64]{};
  };
  const Spine& spine(uint16_t i) const { return items[i]; }

  struct TocEntry {
    char title[80]{};
    char href[96]{};
    uint32_t atomOff = 0xFFFFFFFFu;
    uint16_t spine = 0xFFFFu;
  };
  uint16_t tocCount() const { return nToc; }
  const TocEntry& toc(uint16_t i) const { return tocs[i]; }

 private:
  ZipArchive zip;
  Spine* items = nullptr;
  TocEntry* tocs = nullptr;
  uint16_t nSpine = 0;
  uint16_t nToc = 0;
  uint16_t tocCap = 0;
  char bookTitle[128]{};
  char bookAuthor[64]{};
  char ncxHref[96]{};
  char navHref[96]{};
  WritingMode mode = WritingMode::VerticalRl;
  uint16_t sliceEnd = 0;
  const char* error = "closed";

  bool parseContainer(char* opfName, size_t cap);
  // `rewind` rebinds `sax` to the start of the OPF (memory or a work file).
  bool parseOpf(bool (*rewind)(void* ctx, HtmlSax* sax), void* ctx, const char* opfDir);
  bool parseNcx(HtmlSax& sax);
  bool parseNavDoc(HtmlSax& sax);
  bool loadTocDoc(const char* opfDir);
  void addToc(const char* title, const char* href);
  void bindTocToSpine();
};

}  // namespace ts
