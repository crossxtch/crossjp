#include <Arduino.h>
#include <BoardConfig.h>
#include <Gfx.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <ScratchHeap.h>
#include <TypesetBook.h>
#include <WiFi.h>
#include <builtinFonts/all.h>

#include "core/ScreenManager.h"
#include "core/MappedInput.h"
#include "core/Power.h"
#include "core/ReadingFont.h"
#include "core/Settings.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "network/WifiCredentialStore.h"

#ifndef CROSSJP_VERSION
#define CROSSJP_VERSION "dev"
#endif

// EPUB inflate flushes each block straight into an SD write. On the default
// 8 KB loopTask that path dies with the stack canary (puff dynamic tables +
// FatFile::write + SPI). The Huffman tables are static; this is the rest.
SET_LOOP_TASK_STACK_SIZE(12 * 1024);

Gfx gfx(display);
MappedInput mappedInput(gpio);
ScreenManager screenManager(gfx, mappedInput);

EpdFont ui12RegularFont(&ubuntu_12_medium);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFont uiJpFont(&jp_12);
EpdFontFamily ui12Family(&ui12RegularFont, &ui12BoldFont);
EpdFontFamily ui12BoldFamily(&ui12BoldFont, &ui12BoldFont);
EpdFontFamily uiJpFamily(&uiJpFont);

static const char* wakeupName(HalGPIO::WakeupReason reason) {
  switch (reason) {
    case HalGPIO::WakeupReason::PowerButton:
      return "power";
    case HalGPIO::WakeupReason::AfterFlash:
      return "flash";
    case HalGPIO::WakeupReason::AfterUSBPower:
      return "usb";
    default:
      return "other";
  }
}

// Cards written by the previous firmware name keep settings, fonts, and caches.
static void migrateDataDir() {
  constexpr const char* kOld = "/.crossxtch";
  if (Storage.exists(Settings::kDir) || !Storage.exists(kOld)) {
    return;
  }
  if (Storage.rename(kOld, Settings::kDir)) {
    LOG_INF("MAIN", "Renamed %s -> %s", kOld, Settings::kDir);
  } else {
    LOG_ERR("MAIN", "Could not rename %s -> %s", kOld, Settings::kDir);
  }
}

static void setupDisplayAndFonts() {
  display.begin();
  MappedInput::installBusyWaitPoll();
  gfx.begin();
  gfx.insertFont(FONT_UI, &ui12Family);
  gfx.insertFont(FONT_UI_BOLD, &ui12BoldFamily);
  gfx.setFallbackFont(&uiJpFamily);
}

void setup() {
#ifdef ENABLE_SERIAL_LOG
  logSerial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);
#endif
#endif

  WiFi.mode(WIFI_OFF);

  HalSystem::begin();
  gpio.begin();
  // Scratch hole first, framebuffer second: see ScratchHeap::reserve.
  // WifiSession::end() restarts so this runs again on a clean heap.
  ScratchHeap::reserve(BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  const auto wakeupReason = gpio.getWakeupReason();
  LOG_INF("MAIN", "crossjp " CROSSJP_VERSION " board=%s gpio=%s wake=%s bat=%u%% panic=%d",
          BoardConfig::ACTIVE.name, gpio.deviceIsX3() ? "x3" : "x4", wakeupName(wakeupReason),
          static_cast<unsigned>(powerManager.getBatteryPercentage()), HalSystem::isRebootFromPanic() ? 1 : 0);

  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD init failed");
    setupDisplayAndFonts();
    screenManager.showMessage(uiText::sdCardError);
    return;
  }
  LOG_INF("MAIN", "SD ready");
  migrateDataDir();

  HalSystem::checkPanic();
  settings.load();
  ReadingFont::migrate();
  wifiCredentials.load();
  Frontlight.begin(0, 0, false);

  bool recoveryFirmware = false;
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      if (!gpio.verifyPowerButtonWakeup(800, true)) {
        LOG_INF("MAIN", "Power press too short, sleeping");
        powerManager.startDeepSleep(gpio);
      } else {
        LOG_INF("MAIN", "Power wake, waiting for button release");
        power::noteWakeHold();
        const unsigned long settleStart = millis();
        while (millis() - settleStart < 500) {
          gpio.update();
          delay(10);
        }
        if (gpio.isPressed(HalGPIO::BTN_UP)) {
          recoveryFirmware = true;
          LOG_INF("MAIN", "Recovery firmware mode (UP + POWER)");
        }
      }
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      LOG_INF("MAIN", "USB-power wake, sleeping");
      powerManager.startDeepSleep(gpio);
      break;
    default:
      break;
  }

  setupDisplayAndFonts();
  display.setInverted(settings.nightMode != 0);
  LOG_INF("MAIN", "Display up night=%u", settings.nightMode);

  if (recoveryFirmware) {
    screenManager.goToFirmwareUpdate(true);
    return;
  }

  if (wakeupReason == HalGPIO::WakeupReason::PowerButton && settings.lastBookPath[0] != '\0' &&
      Storage.exists(settings.lastBookPath) && TypesetBook::hasBookExt(settings.lastBookPath)) {
    LOG_INF("MAIN", "Resume %s", settings.lastBookPath);
    screenManager.goToReader(settings.lastBookPath);
  } else {
    LOG_INF("MAIN", "Go home (last='%s')", settings.lastBookPath);
    screenManager.goHome();
  }
}

void loop() {
  mappedInput.update();
  if (power::consumeWakeRelease(gpio)) {
    return;
  }
  power::noteUserActivity(gpio);
  if (power::maybeSleep(gpio, settings)) {
    return;
  }
  if (power::maybeToggleTiltLock(gpio)) {
    return;
  }
  const uint8_t tiltMode = power::tiltLocked() ? CrossPointTiltPageTurn::TILT_OFF : settings.tiltPageTurn;
  halTiltSensor.update(tiltMode, CrossPointOrientation::PORTRAIT, screenManager.isReader());
  screenManager.loop();
  power::idleDelay();
}
