#pragma once

#include <cstdint>

// Sidecar files under /.crossjp keyed by FNV-1a of the book path:
// a_*.bin atoms, t_*.bin page index, c_*.bin chapter TOC, p_*.bin progress.
namespace BookCache {

uint32_t key(const char* path);
void removeFor(const char* bookPath);
// Drops a_*/t_*/c_*/p_*.bin and work.xhtml. Keeps settings.bin and fonts/.
unsigned clearAll();

}  // namespace BookCache
