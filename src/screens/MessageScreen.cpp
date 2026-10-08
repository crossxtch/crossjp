#include "screens/MessageScreen.h"

#include <Gfx.h>

#include <cstring>

#include "core/fontIds.h"

MessageScreen::MessageScreen(Gfx& gfx, MappedInput& input, const char* message) : Screen("Message", gfx, input) {
  snprintf(text, sizeof(text), "%s", message ? message : "");
}

void MessageScreen::loop() {
  if (input.wasReleased(MappedInput::Button::Confirm) || input.wasReleased(MappedInput::Button::Back)) {
    goHome();
  }
}

void MessageScreen::render() {
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI_BOLD, gfx.height() / 2, text);
  presentUi();
}
