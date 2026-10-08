#pragma once

#include <memory>
#include <vector>

#include "core/Screen.h"

class Gfx;

class ScreenManager {
  Gfx& gfx;
  MappedInput& input;
  std::unique_ptr<Screen> current;
  std::vector<std::unique_ptr<Screen>> stack;
  std::unique_ptr<Screen> pending;
  enum class Pending { None, Push, Pop, Replace } pendingAction = Pending::None;
  int pendingPopLevels = 1;
  bool dirty = false;

  void applyPending();

 public:
  ScreenManager(Gfx& gfx, MappedInput& input) : gfx(gfx), input(input) { stack.reserve(8); }

  void loop();
  void requestUpdate() { dirty = true; }
  void replace(std::unique_ptr<Screen> screen);
  void push(std::unique_ptr<Screen> screen);
  // levels > 1 skips intermediate screens (e.g. return straight to the reader
  // from a screen pushed two levels deep) without them ever resuming.
  void pop(int levels = 1);
  void goHome();
  bool goToReader(const char* path);
  bool goToBrowser();
  bool goToSettings();
  void goToFirmwareUpdate(bool recovery = false);
  bool goToWifiFileTransfer();
  void showMessage(const char* text);

  bool isReader() const { return current && current->isReader(); }
  bool blocksSleep() const { return current && current->blocksSleep(); }
};

extern ScreenManager screenManager;
