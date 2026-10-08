#include "core/ScreenManager.h"

#include <HalPowerManager.h>
#include <Logging.h>
#include <Memory.h>

#include "screens/BrowserScreen.h"
#include "screens/HomeScreen.h"

#include "screens/MessageScreen.h"
#include "screens/ReaderScreen.h"
#include "screens/SettingsScreen.h"
#include "screens/WifiListScreen.h"

namespace {
template <typename T, typename... Args>
std::unique_ptr<T> makeScreen(Gfx& gfx, MappedInput& input, const char* tag, Args&&... args) {
  auto screen = makeUniqueNoThrow<T>(gfx, input, std::forward<Args>(args)...);
  if (!screen) {
    LOG_ERR("SCR", "OOM: %s", tag);
  }
  return screen;
}
}  // namespace

void ScreenManager::applyPending() {
  while (pendingAction != Pending::None) {
    input.resetPendingPageTaps();

    if (pendingAction == Pending::Pop) {
      if (current) {
        current->onExit();
        current.reset();
      }
      if (stack.empty()) {
        pendingAction = Pending::None;
        pendingPopLevels = 1;
        goHome();
        continue;
      }
      current = std::move(stack.back());
      stack.pop_back();
      if (--pendingPopLevels > 0) {
        continue;  // more levels queued; keep popping without resuming yet
      }
      pendingAction = Pending::None;
      pendingPopLevels = 1;
      if (current) {
        current->onResume();
      }
      dirty = true;
      continue;
    }

    if (!pending) {
      pendingAction = Pending::None;
      continue;
    }

    if (pendingAction == Pending::Replace) {
      if (current) {
        current->onExit();
        current.reset();
      }
      stack.clear();
    } else if (pendingAction == Pending::Push) {
      if (current) {
        stack.push_back(std::move(current));
      }
    }

    current = std::move(pending);
    pendingAction = Pending::None;
    if (current) {
      current->onEnter();
    }
    dirty = true;
  }
}

void ScreenManager::loop() {
  applyPending();
  if (current) {
    current->loop();
  }
  applyPending();
  if (dirty && current) {
    HalPowerManager::Lock powerLock;
    current->render();
    dirty = false;
  }
}

void ScreenManager::replace(std::unique_ptr<Screen> screen) {
  pending = std::move(screen);
  pendingAction = Pending::Replace;
}

void ScreenManager::push(std::unique_ptr<Screen> screen) {
  pending = std::move(screen);
  pendingAction = Pending::Push;
}

void ScreenManager::pop(const int levels) {
  pendingPopLevels = levels > 1 ? levels : 1;
  pendingAction = Pending::Pop;
}

void ScreenManager::goHome() {
  LOG_INF("SCR", "Home");
  auto home = makeScreen<HomeScreen>(gfx, input, "home");
  if (home) {
    replace(std::move(home));
  }
}

bool ScreenManager::goToReader(const char* path) {
  LOG_INF("SCR", "Reader %s", path ? path : "");
  auto reader = makeScreen<ReaderScreen>(gfx, input, "reader", path);
  if (!reader) {
    return false;
  }
  push(std::move(reader));
  return true;
}

bool ScreenManager::goToBrowser() {
  LOG_INF("SCR", "Browser");
  auto browser = makeScreen<BrowserScreen>(gfx, input, "browser", "/");
  if (!browser) {
    return false;
  }
  push(std::move(browser));
  return true;
}

bool ScreenManager::goToSettings() {
  LOG_INF("SCR", "Settings");
  auto settingsScreen = makeScreen<SettingsScreen>(gfx, input, "settings");
  if (!settingsScreen) {
    return false;
  }
  push(std::move(settingsScreen));
  return true;
}

void ScreenManager::goToFirmwareUpdate(const bool recovery) {
  LOG_INF("SCR", "Firmware picker recovery=%d", recovery ? 1 : 0);
  auto browser = makeScreen<BrowserScreen>(gfx, input, "firmware", "/", BrowserScreen::Mode::Firmware);
  if (!browser) {
    return;
  }
  if (recovery) {
    replace(std::move(browser));
  } else {
    push(std::move(browser));
  }
}

bool ScreenManager::goToWifiFileTransfer() {
  LOG_INF("SCR", "Wi-Fi file transfer");
  auto wifiList = makeScreen<WifiListScreen>(gfx, input, "wifi");
  if (!wifiList) {
    return false;
  }
  push(std::move(wifiList));
  return true;
}

void ScreenManager::showMessage(const char* text) {
  LOG_INF("SCR", "Message: %s", text ? text : "");
  auto message = makeScreen<MessageScreen>(gfx, input, "message", text);
  if (message) {
    replace(std::move(message));
  }
}
