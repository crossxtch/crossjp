#pragma once

#include <HalStorage.h>

#include <cstdint>

namespace ts {

// PIC1. Fixed-size 2-bit pages: header, then id-1 full portrait pages.
// Pixel 0 is white and 3 is black, four pixels per byte, MSB first.
constexpr uint32_t kPictureMagic = 0x31434950u;
constexpr uint16_t kMaxPictures = 48;
constexpr uint32_t kPictureHeader = 16;

class PictureWriter {
 public:
  PictureWriter() = default;
  ~PictureWriter() { end(); }
  PictureWriter(const PictureWriter&) = delete;
  PictureWriter& operator=(const PictureWriter&) = delete;

  bool begin(const char* path, uint16_t pageW, uint16_t pageH);
  void end();
  bool ready() const { return out.isOpen() && pageW > 0; }
  uint16_t count() const { return n; }

  // Stream-decode one baseline JPEG or non-interlaced PNG. Returns a 1-based id, or 0.
  uint16_t addJpeg(HalFile& image);
  uint16_t addPng(HalFile& image);

 private:
  HalFile out;
  uint16_t pageW = 0;
  uint16_t pageH = 0;
  uint16_t n = 0;
  uint16_t rowBytes = 0;
  uint32_t pageBytes = 0;

  bool seekSlot();
  bool writeZeroRows(uint8_t* zeros, uint16_t nrows);
  uint16_t commit();
};

// Copy one stored page into `dst` (pageW * pageH / 4 bytes). Id is 1-based.
bool pictureRead(const char* path, uint16_t id, uint16_t pageW, uint16_t pageH, uint8_t* dst, size_t dstBytes);

}  // namespace ts
