#include "network/WifiSession.h"

#include <Arduino.h>
#include <Gfx.h>
#include <Logging.h>
#include <ScratchHeap.h>

#include "core/UiText.h"
#include "core/fontIds.h"
#include "network/WifiManager.h"

void WifiSession::begin() { ScratchHeap::release(); }

[[noreturn]] void WifiSession::end(Gfx& gfx) {
  wifiManager.disconnect();
  LOG_INF("WIFI", "Restarting to restore reader heap");
  // E-ink holds this frame across ESP.restart() until Home paints.
  gfx.clear(false);
  gfx.drawCenteredText(FONT_UI, gfx.height() / 2, uiText::loading);
  gfx.present(HalDisplay::HALF_REFRESH);
  delay(50);
  ESP.restart();
  for (;;) {
  }
}
