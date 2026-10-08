#pragma once

#include <cstdint>
#include <cstring>

namespace ts {

// Contain-fit into the logical portrait page. Do not scale a smaller image up.
struct FitBox {
  uint16_t w = 0;
  uint16_t h = 0;
  uint16_t x = 0;
  uint16_t y = 0;
};

inline FitBox containFit(const uint16_t srcW, const uint16_t srcH, const uint16_t pageW, const uint16_t pageH) {
  FitBox box{};
  if (srcW == 0 || srcH == 0 || pageW == 0 || pageH == 0) {
    return box;
  }
  if (srcW <= pageW && srcH <= pageH) {
    box.w = srcW;
    box.h = srcH;
  } else if (static_cast<uint64_t>(srcW) * pageH >= static_cast<uint64_t>(srcH) * pageW) {
    box.w = pageW;
    const uint32_t h = static_cast<uint32_t>((static_cast<uint64_t>(srcH) * pageW) / srcW);
    box.h = static_cast<uint16_t>(h == 0 ? 1 : (h > pageH ? pageH : h));
  } else {
    box.h = pageH;
    const uint32_t w = static_cast<uint32_t>((static_cast<uint64_t>(srcW) * pageH) / srcH);
    box.w = static_cast<uint16_t>(w == 0 ? 1 : (w > pageW ? pageW : w));
  }
  box.x = static_cast<uint16_t>((pageW - box.w) / 2);
  box.y = static_cast<uint16_t>((pageH - box.h) / 2);
  return box;
}

// 255 white … 0 black, same direction as JPEG luma. Panel values: 0 white, 3 black.
inline uint8_t quantizeGray(const int v) {
  if (v >= 192) {
    return 0;
  }
  if (v >= 128) {
    return 1;
  }
  if (v >= 64) {
    return 2;
  }
  return 3;
}

inline int quantizeLevel(const uint8_t pix) {
  static const int kLevel[4] = {255, 160, 96, 0};
  return kLevel[pix & 3];
}

// Floyd–Steinberg one row into a 2-bit page row (MSB first, 4 pixels per byte).
// `errCur` is consumed. On return it holds the error to apply to the next row.
inline void ditherRow(const uint8_t* gray, const int n, int16_t* errCur, int16_t* errNext, uint8_t* packed,
                      const int x0) {
  if (!gray || !errCur || !errNext || !packed || n <= 0) {
    return;
  }
  memset(errNext, 0, static_cast<size_t>(n) * sizeof(int16_t));
  for (int x = 0; x < n; ++x) {
    int v = static_cast<int>(gray[x]) + errCur[x];
    if (v < 0) {
      v = 0;
    }
    if (v > 255) {
      v = 255;
    }
    const uint8_t pix = quantizeGray(v);
    const int err = v - quantizeLevel(pix);
    if (x + 1 < n) {
      errCur[x + 1] = static_cast<int16_t>(errCur[x + 1] + err * 7 / 16);
    }
    if (x > 0) {
      errNext[x - 1] = static_cast<int16_t>(errNext[x - 1] + err * 3 / 16);
    }
    errNext[x] = static_cast<int16_t>(errNext[x] + err * 5 / 16);
    if (x + 1 < n) {
      errNext[x + 1] = static_cast<int16_t>(errNext[x + 1] + err * 1 / 16);
    }
    const int px = x0 + x;
    if (px < 0) {
      continue;
    }
    const unsigned shift = 6u - (static_cast<unsigned>(px) & 3u) * 2u;
    packed[static_cast<unsigned>(px) >> 2] =
        static_cast<uint8_t>(packed[static_cast<unsigned>(px) >> 2] | (pix << shift));
  }
  memcpy(errCur, errNext, static_cast<size_t>(n) * sizeof(int16_t));
}

}  // namespace ts
