#pragma once

#include <EpdFontFamily.h>
#include <HalDisplay.h>

#include <cstdint>

// Portrait drawing surface. Panel is landscape (X3 792×528, X4 800×480);
// logical size is swapped (X3 528×792, X4 480×800).
class Gfx {
 public:
  explicit Gfx(HalDisplay& display);

  void begin();

  int width() const { return logicalWidth; }
  int height() const { return logicalHeight; }
  uint16_t stride() const { return panelWidthBytes; }
  int fbWidth() const { return panelWidth; }
  int fbHeight() const { return panelHeight; }

  void insertFont(int id, const EpdFontFamily* family);
  const EpdFontFamily* font(int id) const;
  // Used when the UI face has no glyph (Jōyō + kana packed in firmware).
  void setFallbackFont(const EpdFontFamily* family);

  void clear(bool black = false);
  void drawPixel(int x, int y, bool black);
  void fillRect(int x, int y, int w, int h, bool black);
  void drawText(int fontId, int x, int y, const char* text, bool black = true,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void drawCenteredText(int fontId, int y, const char* text, bool black = true,
                        EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  // One UI-font glyph at the same y as drawText. Returns advance, or 0 if missing.
  int drawCodepoint(int fontId, int x, int y, uint32_t cp, bool black = true,
                    EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  int textWidth(int fontId, const char* text, EpdFontFamily::Style style = EpdFontFamily::REGULAR) const;
  int lineHeight(int fontId) const;

  void present(HalDisplay::RefreshMode mode = HalDisplay::FAST_REFRESH);

  uint8_t* frameBuffer() const { return fb; }

  // 2-bit grayscale helpers used by TypesetBook::drawPage. Framebuffer is scratch between passes.
  void displayGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH);
  void startGrayscaleBase(HalDisplay::RefreshMode fallback = HalDisplay::HALF_REFRESH);
  void finishGrayscaleBase();
  void preconditionGrayscale();
  void copyGrayscaleLsbBuffers();
  void copyGrayscaleMsbBuffers();
  void displayGrayBuffer();
  void startGrayBuffer();
  void finishGrayBuffer();
  void cleanupGrayscaleBuffers();
  bool combinesGrayscaleBase() const;

 private:
  static constexpr int kMaxFonts = 4;

  HalDisplay& panel;
  uint8_t* fb = nullptr;
  uint16_t panelWidth = 0;
  uint16_t panelHeight = 0;
  uint16_t panelWidthBytes = 0;
  int logicalWidth = 0;
  int logicalHeight = 0;

  struct FontSlot {
    int id = 0;
    const EpdFontFamily* family = nullptr;
  };
  FontSlot fonts[kMaxFonts]{};
  const EpdFontFamily* fallbackFont = nullptr;

  void toPanel(int x, int y, int& phyX, int& phyY) const;
  const EpdFontFamily* faceFor(int fontId, uint32_t cp, EpdFontFamily::Style style) const;
  void blitGlyph(const EpdFontData* data, const EpdGlyph* glyph, int gx0, int gy0, bool black);
};
