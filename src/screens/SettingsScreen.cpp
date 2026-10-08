#include "SettingsScreen.h"

#include <Gfx.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <cstdio>

#include <Memory.h>

#include "core/BookCache.h"
#include "core/Settings.h"
#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "screens/FontsScreen.h"
#include "screens/LanguageScreen.h"

#ifndef CROSSJP_VERSION
#define CROSSJP_VERSION "dev"
#endif

namespace {
// Tilt page-turn is only offered on boards with the QMI8658 IMU (X3).
bool hasTilt() { return halTiltSensor.isAvailable(); }
constexpr int kLanguage = 0;
constexpr int kSleep = 1;
constexpr int kRefresh = 2;
constexpr int kNight = 3;
constexpr int kFont = 4;
int itemCount() { return hasTilt() ? 10 : 8; }
int tiltIndex() { return 5; }
int gyroIndex() { return 6; }
int cacheIndex() { return hasTilt() ? 7 : 5; }
int firmwareIndex() { return hasTilt() ? 8 : 6; }

void formatTrueSleep(char* out, size_t outSize) {
  if (settings.trueSleepMinutes == Settings::kSleepNone) {
    snprintf(out, outSize, "%s", uiText::powerOffNone);
  } else {
    snprintf(out, outSize, uiText::powerOffMin, settings.trueSleepMinutes);
  }
}

void formatGyroAutoOff(char* out, size_t outSize) {
  if (settings.gyroAutoOffSeconds == 0) {
    snprintf(out, outSize, "%s", uiText::gyroAutoOffNone);
  } else {
    snprintf(out, outSize, uiText::gyroAutoOffSec, settings.gyroAutoOffSeconds);
  }
}

void bumpGyroAutoOff() {
  switch (settings.gyroAutoOffSeconds) {
    case 0:
      settings.gyroAutoOffSeconds = 30;
      break;
    case 30:
      settings.gyroAutoOffSeconds = 45;
      break;
    case 45:
      settings.gyroAutoOffSeconds = 60;
      break;
    default:
      settings.gyroAutoOffSeconds = 0;
      break;
  }
}

void bumpTrueSleep() {
  switch (settings.trueSleepMinutes) {
    case Settings::kSleepNone:
      settings.trueSleepMinutes = Settings::kSleep5Min;
      break;
    case Settings::kSleep5Min:
      settings.trueSleepMinutes = Settings::kSleep10Min;
      break;
    case Settings::kSleep10Min:
      settings.trueSleepMinutes = Settings::kSleep15Min;
      break;
    default:
      settings.trueSleepMinutes = Settings::kSleepNone;
      break;
  }
}

char refreshBuf[48];

const char* refreshLabel() {
  if (settings.refreshEveryNPages == 1) {
    return uiText::refreshEveryPage;
  }
  snprintf(refreshBuf, sizeof(refreshBuf), uiText::refreshEveryN, settings.refreshEveryNPages);
  return refreshBuf;
}

void bumpRefresh() {
  switch (settings.refreshEveryNPages) {
    case 1:
      settings.refreshEveryNPages = 5;
      break;
    case 5:
      settings.refreshEveryNPages = 10;
      break;
    case 10:
      settings.refreshEveryNPages = 15;
      break;
    case 15:
      settings.refreshEveryNPages = 20;
      break;
    default:
      settings.refreshEveryNPages = 1;
      break;
  }
}
}  // namespace

void SettingsScreen::loop() {
  if (input.wasReleased(MappedInput::Button::Back)) {
    settings.save();
    finish();
    return;
  }
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (halClock.getLocalTime(hour, minute, settings.clockUtcOffsetQ) && minute != shownMinute) {
    requestUpdate();
  }
  if (ui::applyDelta(index, input.consumeNavigationDelta(), itemCount())) {
    cacheArmed = false;
    requestUpdate();
  } else if (input.wasReleased(MappedInput::Button::Confirm)) {
    if (index == kLanguage) {
      auto screen = makeUniqueNoThrow<LanguageScreen>(gfx, input, false);
      if (!screen) {
        LOG_ERR("SET", "OOM: language");
        return;
      }
      push(std::move(screen));
      return;
    } else if (index == kSleep) {
      bumpTrueSleep();
      char sleepLog[48];
      formatTrueSleep(sleepLog, sizeof(sleepLog));
      LOG_INF("SET", "%s", sleepLog);
    } else if (index == kRefresh) {
      bumpRefresh();
      LOG_INF("SET", "%s", refreshLabel());
    } else if (index == kNight) {
      settings.nightMode = settings.nightMode ? 0 : 1;
      display.setInverted(settings.nightMode != 0);
      LOG_INF("SET", "Night mode %s", settings.nightMode ? "on" : "off");
    } else if (index == kFont) {
      auto screen = makeUniqueNoThrow<FontsScreen>(gfx, input);
      if (!screen) {
        LOG_ERR("SET", "OOM: fonts");
        return;
      }
      push(std::move(screen));
      return;
    } else if (hasTilt() && index == tiltIndex()) {
      settings.tiltPageTurn = settings.tiltPageTurn ? 0 : 1;
      LOG_INF("SET", "Tilt page turn %s", settings.tiltPageTurn ? "on" : "off");
    } else if (hasTilt() && index == gyroIndex()) {
      bumpGyroAutoOff();
      LOG_INF("SET", "Gyro auto-off %u sec", settings.gyroAutoOffSeconds);
    } else if (index == cacheIndex()) {
      if (!cacheArmed) {
        cacheArmed = true;
        cacheCleared = false;
      } else {
        BookCache::clearAll();
        cacheArmed = false;
        cacheCleared = true;
      }
    } else if (index == firmwareIndex()) {
      settings.save();
      goToFirmwareUpdate();
      return;
    } else {
      settings.save();
      finish();
      return;
    }
    settings.save();
    requestUpdate();
  }
}

void SettingsScreen::render() {
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

  char lang[48];
  char deep[48];
  char night[48];
  char tilt[48];
  char gyro[48];
  snprintf(lang, sizeof(lang), uiText::language, uiText::languageName);
  formatTrueSleep(deep, sizeof(deep));
  snprintf(night, sizeof(night), uiText::nightMode, settings.nightMode ? uiText::on : uiText::off);
  snprintf(tilt, sizeof(tilt), uiText::tiltPageTurn, settings.tiltPageTurn ? uiText::on : uiText::off);
  formatGyroAutoOff(gyro, sizeof(gyro));

  const char* cacheLabel =
      cacheArmed ? uiText::clearCacheConfirm : (cacheCleared ? uiText::cacheCleared : uiText::clearCache);
  const char* labels[10];
  labels[0] = lang;
  labels[1] = deep;
  labels[2] = refreshLabel();
  labels[3] = night;
  labels[4] = uiText::readingFont;
  if (hasTilt()) {
    labels[5] = tilt;
    labels[6] = gyro;
    labels[7] = cacheLabel;
    labels[8] = uiText::updateFirmware;
    labels[9] = uiText::back;
  } else {
    labels[5] = cacheLabel;
    labels[6] = uiText::updateFirmware;
    labels[7] = uiText::back;
  }

  const int rowH = gfx.lineHeight(FONT_UI) + 10;
  const int startY = 90;
  for (int i = 0; i < itemCount(); ++i) {
    ui::drawMenuRow(gfx, startY + i * rowH, rowH, labels[i], i == index);
  }

  gfx.drawCenteredText(FONT_UI, gfx.height() - 40, "crossjp " CROSSJP_VERSION);
  presentUi();
}
