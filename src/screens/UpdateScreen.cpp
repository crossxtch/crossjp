#include "screens/UpdateScreen.h"

#include <Arduino.h>
#include <Gfx.h>
#include <HalDisplay.h>
#include <Logging.h>
#include <esp_system.h>

#include <cstdio>
#include <cstring>

#include "core/UiText.h"
#include "core/fontIds.h"

namespace {
const char* fileName(const char* path) {
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}
}  // namespace

UpdateScreen::UpdateScreen(Gfx& gfx, MappedInput& input, const char* path) : Screen("Update", gfx, input) {
  snprintf(firmwarePath, sizeof(firmwarePath), "%s", path ? path : "");
}

void UpdateScreen::onEnter() {
  Screen::onEnter();
  check = sdUpdate::inspect(firmwarePath);
  if (!check.ok) {
    error = check.error ? check.error : "invalid firmware";
    state = State::Failed;
    LOG_ERR("FW", "Inspect %s: %s", firmwarePath, error);
  }
  requestUpdate();
}

void UpdateScreen::loop() {
  if (state == State::Confirm) {
    if (input.wasReleased(MappedInput::Button::Back)) {
      finish();
      return;
    }
    if (input.wasReleased(MappedInput::Button::Confirm)) {
      state = State::Updating;
      requestUpdate();
      return;
    }
  } else if (state == State::Updating && !started) {
    started = true;
    gfx.clear(false);
    gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2 - 16, uiText::updatingFirmware);
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 16, uiText::doNotPowerOff);
    gfx.present(HalDisplay::HALF_REFRESH);
    if (!sdUpdate::flash(firmwarePath)) {
      error = sdUpdate::lastError();
      if (!error || error[0] == '\0') {
        error = "write failed";
      }
      state = State::Failed;
      requestUpdate();
      return;
    }
    gfx.clear(false);
    gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2, uiText::updateComplete);
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 28, uiText::restarting);
    gfx.present(HalDisplay::HALF_REFRESH);
    delay(800);
    esp_restart();
  } else if (state == State::Failed) {
    if (input.wasReleased(MappedInput::Button::Back) || input.wasReleased(MappedInput::Button::Confirm)) {
      finish();
    }
  }
}

void UpdateScreen::render() {
  if (state == State::Updating) {
    return;
  }
  gfx.clear(false);
  if (state == State::Failed) {
    gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2 - 16, uiText::updateFailed);
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2 + 16, uiText::error(error));
  } else {
    gfx.drawCenteredText(FONT_UI_BOLD, 36, uiText::updateFirmwareQ);
    gfx.drawCenteredText(FONT_UI, 100, fileName(firmwarePath));
    char sizeLine[32];
    snprintf(sizeLine, sizeof(sizeLine), "%u KB", static_cast<unsigned>(check.size / 1024));
    gfx.drawCenteredText(FONT_UI, 130, sizeLine);
    gfx.drawCenteredText(FONT_UI, gfx.height() - 80, uiText::confirmToFlash);
    gfx.drawCenteredText(FONT_UI, gfx.height() - 52, uiText::backToCancel);
  }
  gfx.present(HalDisplay::FAST_REFRESH);
}
