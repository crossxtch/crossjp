#include "screens/FontsScreen.h"

#include <EpdFontFamily.h>
#include <Gfx.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/ReadingFont.h"
#include "core/Settings.h"
#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"

FontsScreen::FontsScreen(Gfx& gfx, MappedInput& input) : Screen("Fonts", gfx, input) {}

void FontsScreen::load() {
  names.clear();
  names.reserve(32);
  HalFile dir = Storage.open(ReadingFont::kDir);
  if (!dir || !dir.isDirectory()) {
    LOG_INF("FONT", "No fonts dir");
    index = 0;
    window = 0;
    return;
  }
  char name[HalFile::kMaxNameBytes];
  for (HalFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory() || file.getName(name, sizeof(name)) == 0) {
      continue;
    }
    if (!ReadingFont::isFontFilename(name)) {
      continue;
    }
    names.emplace_back(name);
    if (names.size() >= 32) {
      break;
    }
  }
  std::sort(names.begin(), names.end());
  if (settings.fontFile[0]) {
    for (size_t i = 0; i < names.size(); ++i) {
      if (names[i] == settings.fontFile) {
        index = static_cast<int>(i);
        break;
      }
    }
  }
  if (index >= static_cast<int>(names.size())) {
    index = 0;
  }
  window = 0;
  LOG_INF("FONT", "%u installed, active='%s'", static_cast<unsigned>(names.size()), settings.fontFile);
}

void FontsScreen::loadCjk() {
  if (cjk.loaded()) {
    return;
  }
  if (!ReadingFont::loadUi(cjk)) {
    LOG_INF("FONT", "No UI font");
  }
}

void FontsScreen::onEnter() {
  Screen::onEnter();
  ReadingFont::migrate();
  loadCjk();
  load();
  requestUpdate();
}

void FontsScreen::onExit() {
  cjk.close();
  Screen::onExit();
}

void FontsScreen::activate() {
  if (names.empty()) {
    return;
  }
  if (!ReadingFont::setActive(names[static_cast<size_t>(index)].c_str())) {
    return;
  }
  finish();
}

void FontsScreen::loop() {
  if (input.wasReleased(MappedInput::Button::Back)) {
    finish();
    return;
  }
  const int count = static_cast<int>(names.size());
  if (ui::applyDelta(index, input.consumeNavigationDelta(), count)) {
    requestUpdate();
  } else if (count > 0 && input.wasReleased(MappedInput::Button::Confirm)) {
    activate();
  }
}

void FontsScreen::render() {
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, 8, uiText::readingFont);

  const int rowH = gfx.lineHeight(FONT_UI) + 8;
  const int top = 40;
  const int rows = (gfx.height() - top - 24) / rowH;
  if (names.empty()) {
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2, uiText::noFonts);
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 28, uiText::uploadFontHint);
    presentUi();
    return;
  }

  ui::followWindow(window, index, rows);
  const int last = std::min(window + rows, static_cast<int>(names.size()));
  char row[96];
  auto fillRow = [&](const int i) {
    const char* name = names[static_cast<size_t>(i)].c_str();
    const bool on = settings.fontFile[0] && strcmp(name, settings.fontFile) == 0;
    snprintf(row, sizeof(row), "%s%s", on ? "* " : "", name);
  };
  if (cjk.loaded()) {
    const EpdFontFamily* ui = gfx.font(FONT_UI);
    uint16_t ids[192];
    uint16_t n = 0;
    for (int i = window; i < last; ++i) {
      fillRow(i);
      const unsigned char* p = reinterpret_cast<const unsigned char*>(row);
      uint32_t cp = 0;
      while ((cp = utf8NextCodepoint(&p)) && n < 192) {
        if (ui && ui->hasCodepoint(cp)) {
          continue;
        }
        const uint16_t id = cjk.glyphId(cp);
        if (id != 0xFFFF) {
          ids[n++] = id;
        }
      }
    }
    cjk.prewarm(ids, n, false);
  }
  for (int i = window; i < last; ++i) {
    fillRow(i);
    ui::drawRow(gfx, top + (i - window) * rowH, rowH, row, i == index, cjk.loaded() ? &cjk : nullptr);
  }
  presentUi();
}
