---
name: hal-and-abstractions
description: "Layering and abstraction discipline for the firmware. Use when touching storage, input, display, settings, or rendering, or any code that could reach into the SDK. Covers routing through the HAL (HalStorage / HalGPIO / HalDisplay) instead of raw SDK classes, MappedInput logical buttons instead of raw GPIO indices, Gfx for all drawing, uiText for user-facing copy, and where a new abstraction boundary belongs."
---

# HAL and Abstractions

## Route through the layer, always

- **SD card I/O:** `Storage` (`HalStorage`) and `HalFile`. Never `SdFat`,
  `FsFile`, or `SDCardManager` from `src/` or from `lib/Typesetter`. The HAL
  serializes every SD access through one mutex; bypassing it races the SPI
  state machine and panics FreeRTOS. `lib/SDCardManager` is the vendored place
  those types live. `lib/EInkDisplay` is the same kind of vendor drop: patch it
  for a panel bug, and do not edit the `freeink-sdk` submodule.
- **Display:** draw through `Gfx`. Refresh modes go through `HalDisplay`.
  `EInkDisplay` stays inside `lib/hal` and `lib/EInkDisplay`.
- **Input in screens:** `MappedInput::Button` (`Back`, `Confirm`, `Left`,
  `Right`, `Up`, `Down`, `Power`, `PageBack`, `PageForward`). The power-button
  sleep path in `core/Power` and the recovery chord in `main` use `HalGPIO`
  because they are the physical button, not a remapped reading action.
- **Rendering:** `Gfx` and `gfx.width()` / `gfx.height()`. UI text uses
  `FONT_UI` and `FONT_UI_BOLD` from `fontIds.h`. The logical page is portrait
  (X3 528×792, X4 480×800); the panel buffer is landscape.
- **Shared objects:** `settings`, `Storage`, `screenManager`, `gpio`,
  `display`, `powerManager`, `halClock`, `halTiltSensor`, `wifiManager`,
  `wifiCredentials`. Pass a dependency only when a free function cannot see
  the object it needs.

## User-facing text

Every string a reader sees is a `uiText::` name from `src/core/UiText.h`.
Add a row to `UI_STRINGS`. The copy is Japanese; Latin stays only where the
string already needs it (Wi-Fi, `.bin`, `.xgf2`). CJK has to fit the firmware
`jp_12` face. Log lines (`LOG_*`) stay hardcoded English.

## Drawing a new boundary

When you need an SDK capability the HAL does not expose yet, **add the method
to the HAL; do not reach around it.** The new method inherits the mutex,
logging, and error contract the rest of the HAL carries.

Keep abstractions thin. A wrapper that only renames an SDK call without adding
the mutex, logging, or an error contract is dead weight. Add a layer only when
it carries one of those contracts or hides a real implementation choice.

## Self-review

- [ ] No direct SdFat / FsFile / SDCardManager / EInkDisplay use outside
      `lib/hal`, `lib/SDCardManager`, and `lib/EInkDisplay`.
- [ ] File access uses `HalFile`. A local `HalFile` closes in its destructor
      (`DESTRUCTOR_CLOSES_FILE`). Members close in `onExit`.
- [ ] Screen input uses `MappedInput::Button`.
- [ ] Drawing goes through `Gfx` and `gfx.width()` / `gfx.height()`.
- [ ] User-facing strings are `uiText::` names, added to `UI_STRINGS`.
- [ ] Any new SDK capability is a HAL method, not an inline SDK call.
