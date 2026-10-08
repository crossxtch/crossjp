#include "screens/HomeScreen.h"

#include <Gfx.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <TypesetBook.h>
#include <XgfFont.h>

#include <cstdio>

#include "core/ReadingFont.h"
#include "core/Settings.h"
#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"

void HomeScreen::refreshMenu() {
  const bool hasContinue = settings.lastBookPath[0] != '\0' && Storage.exists(settings.lastBookPath) &&
                           TypesetBook::hasBookExt(settings.lastBookPath);
  itemCount = hasContinue ? 4 : 3;
  if (index >= itemCount) {
    index = 0;
  }
}

void HomeScreen::loadCjk() {
  if (itemCount != 4) {
    cjk.close();
    return;
  }
  if (cjk.loaded()) {
    return;
  }
  if (!ReadingFont::loadUi(cjk)) {
    LOG_INF("HOME", "No UI font (%s)", cjk.lastError());
  }
}

void HomeScreen::onEnter() {
  Screen::onEnter();
  refreshMenu();
  loadCjk();
  LOG_INF("HOME", "Continue %s last='%s'", itemCount == 4 ? "yes" : "no", settings.lastBookPath);
  requestUpdate();
}

void HomeScreen::onExit() {
  cjk.close();
  Screen::onExit();
}

void HomeScreen::onResume() {
  Screen::onResume();
  refreshMenu();
  loadCjk();
}

void HomeScreen::loop() {
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (halClock.getLocalTime(hour, minute, settings.clockUtcOffsetQ) && minute != shownMinute) {
    requestUpdate();
  }
  if (ui::applyDelta(index, input.consumeNavigationDelta(), itemCount)) {
    requestUpdate();
  } else if (input.wasReleased(MappedInput::Button::Confirm)) {
    const bool hasContinue = itemCount == 4;
    // Item order: [Continue?], Browse, File Transfer, Settings.
    int i = index - (hasContinue ? 1 : 0);
    // Drop the UI face before pushing a screen that may load the same .xgf2
    // (reader, browser) or needs the heap (Wi-Fi).
    cjk.close();
    bool ok = true;
    if (hasContinue && index == 0) {
      LOG_DBG("HOME", "Continue");
      ok = goToReader(settings.lastBookPath);
    } else if (i == 0) {
      LOG_DBG("HOME", "Browse");
      ok = goToBrowser();
    } else if (i == 1) {
      LOG_DBG("HOME", "File Transfer");
      ok = goToWifiFileTransfer();
    } else {
      LOG_DBG("HOME", "Settings");
      ok = goToSettings();
    }
    if (!ok) {
      loadCjk();
    }
  }
}

void HomeScreen::render() {
  gfx.clear(false);

  char clock[8];
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (halClock.getLocalTime(hour, minute, settings.clockUtcOffsetQ)) {
    snprintf(clock, sizeof(clock), "%02u:%02u", hour, minute);
    gfx.drawCenteredText(FONT_UI_BOLD, 8, clock);
    shownMinute = minute;
  }

  char bat[16];
  snprintf(bat, sizeof(bat), "%u%%", static_cast<unsigned>(powerManager.getBatteryPercentage()));
  gfx.drawText(FONT_UI, gfx.width() - gfx.textWidth(FONT_UI, bat) - 12, 8, bat);

  const bool hasContinue = itemCount == 4;
  const char* labels[4];
  int n = 0;
  if (hasContinue) {
    labels[n++] = uiText::continueReading;
  }
  labels[n++] = uiText::browse;
  labels[n++] = uiText::fileTransfer;
  labels[n++] = uiText::settings;

  const int rowH = gfx.lineHeight(FONT_UI) + 10;
  const int startY = 120;
  for (int i = 0; i < n; ++i) {
    ui::drawMenuRow(gfx, startY + i * rowH, rowH, labels[i], i == index);
  }

  if (hasContinue) {
    cjk.drawUtf8(gfx, FONT_UI, 24, gfx.height() - 48, settings.lastBookPath, true, gfx.width() - 24);
  }
  presentUi();
}
