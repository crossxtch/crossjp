#pragma once

#include <cstdint>

// Sidecar files under /.crossjp keyed by FNV-1a of the book path:
// a_*.bin atoms, t_*.bin page index, c_*.bin chapter TOC, p_*.bin progress, s_*.bin spine cursor.
namespace BookCache {

uint32_t key(const char* path);
// a_ atom, b_ alternate atom, t_ page index, c_ chapters, p_ progress, i_ pictures, s_ spine cursor.
void removeFor(const char* bookPath);
// Those sidecars, plus work.xhtml / work.opf / work.toc / work.img / work.idx.
// Keeps settings.bin and fonts/.
unsigned clearAll();

}  // namespace BookCache
