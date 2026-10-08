#pragma once

#include <ChapterInfo.h>
#include <XgfFont.h>

#include <vector>

#include "core/Screen.h"

class ReaderScreen;

class ChapterSelectionScreen final : public Screen {
  ReaderScreen& reader;
  // The open book's list. Copying every title allocates again and abort()s
  // once the font cache has split the heap (exceptions are disabled).
  const std::vector<ts::ChapterInfo>& chapters;
  uint32_t currentPage;
  uint16_t pageCount;
  int index = 0;
  int window = 0;
  XgfFont owned;
  XgfFont* face = nullptr;

  void activate();

 public:
  ChapterSelectionScreen(Gfx& gfx, MappedInput& input, ReaderScreen& reader,
                         const std::vector<ts::ChapterInfo>& chapterList, uint32_t currentPage,
                         uint16_t pageCount);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render() override;
};
