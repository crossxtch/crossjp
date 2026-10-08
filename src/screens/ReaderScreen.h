#pragma once

#include <TypesetBook.h>

#include <memory>
#include <vector>

#include "core/Screen.h"

class ReaderScreen final : public Screen {
  char bookPath[256]{};
  // Heap-allocated: TypesetBook holds ~24 KB of glyph-run arrays, and
  // BrowserScreen is still alive when the reader opens.
  std::unique_ptr<TypesetBook> book;
  uint32_t page = 0;
  uint32_t resumeAtom = 0;
  int pagesUntilFull = 0;
  bool loaded = false;
  bool painted = false;
  const char* openError = "out of memory";

  unsigned long lastOpenProgressMs = 0;

  void loadProgress();
  void saveProgress() const;
  void clampPage();
  uint32_t viewPage() const;
  void showStatus(const char* title, const char* detail = nullptr);
  void showOpenProgress(uint16_t done, uint16_t total);
  static void onOpenProgress(void* ctx, uint16_t done, uint16_t total);
  uint16_t bookPageCount() const;
  const char* bookError() const;
  const std::vector<ts::ChapterInfo>& bookChapters();

 public:
  ReaderScreen(Gfx& gfx, MappedInput& input, const char* path);
  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void loop() override;
  void render() override;
  bool isReader() const override { return true; }

  void jumpToPage(uint32_t targetPage);
  void openChapter(uint16_t chapterIndex);
  XgfFont* cjkFont();
};
