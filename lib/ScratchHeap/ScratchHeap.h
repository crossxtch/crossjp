#pragma once

#include <cstdint>

// Contiguous DRAM hole reserved before the framebuffer splits the heap.
// Typesetting and Wi-Fi borrow it. Getting it back takes a restart.
namespace ScratchHeap {

bool reserve(uint16_t width, uint16_t height);
void release();

}  // namespace ScratchHeap
