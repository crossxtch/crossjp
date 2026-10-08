#include "Kinsoku.h"

#include <cstddef>

namespace ts {
namespace {

constexpr uint32_t kNotStart[] = {
    0x203C, 0x2047, 0x2048, 0x2049,  // ‼ ⁇ ⁈ ⁉
    0x3001, 0x3002, 0x3009, 0x300B, 0x300D, 0x300F, 0x3011,  // 、。〉》」』】
    0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308E,  // ぁぃぅぇぉっゃゅょゎ
    0x309D, 0x309E,  // ゝゞ
    0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x30F5, 0x30F6,  // ァ..ヶ
    0x30FB, 0x30FC,  // ・ー
    0xFF01, 0xFF09, 0xFF0C, 0xFF0E, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF3D, 0xFF5D,
};

constexpr uint32_t kNotEnd[] = {
    0x3008, 0x300A, 0x300C, 0x300E, 0x3010,  // 〈《「『【
    0xFF08, 0xFF3B, 0xFF5B,                  // （［｛
};

bool containsSorted(const uint32_t* table, const size_t n, const uint32_t cp) {
  size_t lo = 0;
  size_t hi = n;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (table[mid] < cp) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo < n && table[lo] == cp;
}

}  // namespace

bool kinsokuCanStartColumn(const uint32_t cp) {
  return !containsSorted(kNotStart, sizeof(kNotStart) / sizeof(kNotStart[0]), cp);
}

bool kinsokuCanEndColumn(const uint32_t cp) {
  return !containsSorted(kNotEnd, sizeof(kNotEnd) / sizeof(kNotEnd[0]), cp);
}

bool kinsokuHangs(const uint32_t cp) {
  return cp == 0x3001 || cp == 0x3002 || cp == 0xFF0C || cp == 0xFF0E;
}

bool kinsokuHalfAdvance(const uint32_t cp) {
  return kinsokuHangs(cp) || cp == 0x30FB || cp == 0xFF1A;
}

int16_t kinsokuInlineAdvance(const uint32_t cp, const int16_t em, const bool tcy) {
  if (tcy || em <= 1 || !kinsokuHalfAdvance(cp)) {
    return em;
  }
  const int16_t half = static_cast<int16_t>(em / 2);
  return half < 1 ? static_cast<int16_t>(1) : half;
}

bool shouldRotate(const uint32_t v) {
  if (v >= 0x30 && v <= 0x39) {
    return false;
  }
  if (v >= 0xFF10 && v <= 0xFF19) {
    return false;
  }
  if (v >= 0x41 && v <= 0x5A) {
    return true;
  }
  if (v >= 0x61 && v <= 0x7A) {
    return true;
  }
  if (v >= 0x00C0 && v <= 0x024F) {
    return true;
  }
  if (v >= 0x21 && v <= 0x2F) {
    return true;
  }
  if (v >= 0x3A && v <= 0x40) {
    return true;
  }
  if (v >= 0x5B && v <= 0x60) {
    return true;
  }
  if (v >= 0x7B && v <= 0x7E) {
    return true;
  }
  switch (v) {
    case 0x00B7:
    case 0x2010:
    case 0x2013:
    case 0x2014:
    case 0x2015:
    case 0x2018:
    case 0x2019:
    case 0x201C:
    case 0x201D:
    case 0x2025:
    case 0x2026:
    case 0x22EF:
    case 0x2212:
    case 0x30A0:
    case 0x30FC:
    case 0x3008:
    case 0x3009:
    case 0x300A:
    case 0x300B:
    case 0x3010:
    case 0x3011:
    case 0x3014:
    case 0x3015:
    case 0x3016:
    case 0x3017:
    case 0x301C:
    case 0x3030:
    case 0xFF08:
    case 0xFF09:
    case 0xFF3B:
    case 0xFF3D:
    case 0xFF5B:
    case 0xFF5D:
    case 0xFF0D:
    case 0xFF1D:
    case 0xFF5E:
    case 0xFF70:
      return true;
    default:
      return false;
  }
}

}  // namespace ts
