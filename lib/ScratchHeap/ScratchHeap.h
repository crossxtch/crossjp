#pragma once

#include <cstddef>
#include <cstdint>

// Contiguous DRAM hole reserved before the framebuffer splits the heap.
// Typesetting and Wi-Fi borrow it. Getting it back takes a restart.
namespace ScratchHeap {

bool reserve(uint16_t width, uint16_t height);
void release();

// The reserved hole. Null after release. Callers may write into it and must not free it.
uint8_t* data();
size_t size();

}  // namespace ScratchHeap
