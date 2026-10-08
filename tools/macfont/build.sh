#!/bin/bash
# Build Crossjp Font.app and run the encoder self-test. No device flash.
set -euo pipefail
cd "$(dirname "$0")"

SDK="$(xcrun --sdk macosx --show-sdk-path)"
TARGET="arm64-apple-macosx13.0"
APP="Crossjp Font.app"

echo "self-test"
swiftc -swift-version 5 -O -sdk "$SDK" -target "$TARGET" \
  -framework AppKit -framework CoreText -framework CoreGraphics \
  Sources/XgfEncode.swift Sources/Rasterizer.swift Sources/FaceCatalog.swift Sources/SelfTestMain.swift \
  -o /tmp/crossjp-font-selftest
/tmp/crossjp-font-selftest

python3 - <<'PY'
import struct
p = "/tmp/crossjp-font-sample.xgf2"
d = open(p, "rb").read()
magic, ver, flags, em, ruby, bpp, _r0, bs, rs, bc, rc, ic, _r1, ioff, rmoff, boff, roff = struct.unpack_from(
    "<IHHBBBBHHHHHHIIII", d, 0
)
assert magic == 0x32464758 and ver == 1 and flags == 7
assert em == 16 and ruby == 8 and bpp == 2
assert ioff == 64 and bs == 64 and rs == 16 and bc == 4 and rc == 3 and ic == 2
assert rmoff == 76 and boff == 512 and roff == 1024
assert d[512] == 0x11 and d[512 + 64] == 0x11 and d[1024] == 0x22
assert len(d) == 1072
print("python header ok", len(d))
PY

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp Info.plist "$APP/Contents/Info.plist"
cp ../joyo_kanji.txt "$APP/Contents/Resources/joyo_kanji.txt"

echo "app"
swiftc -swift-version 5 -O -sdk "$SDK" -target "$TARGET" \
  -framework SwiftUI -framework AppKit -framework CoreText -framework CoreGraphics -framework UniformTypeIdentifiers \
  Sources/XgfEncode.swift Sources/Rasterizer.swift Sources/FaceCatalog.swift Sources/ContentView.swift Sources/AppMain.swift \
  -o "$APP/Contents/MacOS/CrossjpFont"
codesign --force --sign - "$APP"
echo "built $(pwd)/$APP"
