#include "ScratchHeap.h"

#include <Arduino.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <cstdlib>

namespace {

uint8_t* block = nullptr;
size_t capacity = 0;

constexpr size_t kGrowthBlock = 4096;
// Packed page header the old XTCH blit reserved in front of the bitmap.
// 4 KiB rounding means dropping these 22 bytes can shrink the hole by a block.
constexpr size_t kHeaderSlack = 22;

size_t roundUp(const size_t size) { return (size + (kGrowthBlock - 1)) & ~(kGrowthBlock - 1); }

size_t bytesFor(const uint16_t width, const uint16_t height) {
  const size_t colBytes = (static_cast<size_t>(height) + 7) / 8;
  const size_t bitmap = colBytes * static_cast<size_t>(width) * 2;
  return roundUp(bitmap + kHeaderSlack);
}

void logHeap(const char* what) {
  LOG_INF("HEAP", "%s: freeHeap=%u largestFreeBlock=%u", what, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

}  // namespace

bool ScratchHeap::reserve(const uint16_t width, const uint16_t height) {
  const size_t need = bytesFor(width, height);
  if (block != nullptr && capacity >= need) {
    return true;
  }
  uint8_t* next = static_cast<uint8_t*>(malloc(need));
  if (!next) {
    LOG_ERR("HEAP", "Failed to reserve scratch (%lu bytes): freeHeap=%u largestFreeBlock=%u",
            static_cast<unsigned long>(need), static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
    return false;
  }
  free(block);
  block = next;
  capacity = need;
  logHeap("Reserved scratch");
  return true;
}

void ScratchHeap::release() {
  free(block);
  block = nullptr;
  capacity = 0;
  logHeap("Released scratch");
}
