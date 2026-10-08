#pragma once

#include <cstdint>

namespace ts {

bool kinsokuCanStartColumn(uint32_t cp);
bool kinsokuCanEndColumn(uint32_t cp);
bool shouldRotate(uint32_t cp);

// 。、．， may sit past the 版面. Brackets do not.
bool kinsokuHangs(uint32_t cp);
// Small marks only. XGF ink is centered in the em, so a tall bracket
// compressed to half an em would collide with its neighbors.
bool kinsokuHalfAdvance(uint32_t cp);
int16_t kinsokuInlineAdvance(uint32_t cp, int16_t em, bool tcy);

inline bool isHiragana(const uint32_t cp) { return cp >= 0x3040 && cp <= 0x309F; }
inline bool isKatakana(const uint32_t cp) { return cp >= 0x30A0 && cp <= 0x30FF; }
inline bool isKanji(const uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0xF900 && cp <= 0xFAFF);
}

}  // namespace ts
