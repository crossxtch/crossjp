# crossjp

EPUB and plain-text reader for the Xteink X3 and X4. Books are `.epub` and `.txt` files on the SD card. This firmware typesets them. It does not read `.xtch` or `.xtc`.

One firmware binary per device. Pages are rendered for that panel:

| Device | Panel (landscape) | UI / pages (portrait) |
| ------ | ----------------- | --------------------- |
| X3     | 792×528           | 528×792               |
| X4     | 800×480           | 480×800               |

## Layout

```
src/main.cpp          boot + loop
src/core/             screen stack, input, settings, power, list UI
src/screens/          home, browser, reader, chapters, settings, update
src/platform/         chip workarounds
lib/hal               hardware wrappers (still CrossPoint-shaped)
lib/Gfx               portrait framebuffer + UI text
lib/Typesetter        EPUB and UTF-8 text layout
lib/XgfFont           reading font
lib/EpdFont           Ubuntu UI fonts
reference/            CrossPoint submodule (lookup only)
freeink-sdk/          display and board support
```

Settings and per-book progress live in `/.crossjp` on the SD card.
Boot and runtime logs go to the Serial console only (no SD log file).

Firmware update from SD (same idea as CrossPoint):

- Settings → **Update firmware** → pick a `.bin` → confirm.
- Recovery picker: hold **up + power** at wake. Back returns to home.

Flash the matching env's `.bin` (X3 firmware on an X4, or the reverse, will not drive the panel). The stock Xteink updater is gone once this firmware is installed.

## Build

Default env is `x3`. Separate binary per device — do not combine them.

```bash
pio run              # env:x3
pio run -e x4
pio run -e x3 -t upload
pio device monitor
```

## Release

Bump `[crossjp] version` in `platformio.ini` to match the tag, push `develop`, then:

```bash
git tag -a v0.1.0 -m "v0.1.0"
git push origin v0.1.0
```

GitHub Actions builds `crossjp-x3.bin` and `crossjp-x4.bin` and attaches them to a GitHub Release for that tag.

Optional `platformio.local.ini` for a local upload port (gitignored).

`reference/` is the CrossPoint Reader tree. Do not build it as this product.

```bash
git submodule update --init --recursive
```
