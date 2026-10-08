#include "PicturePage.h"

#include "PictureDither.h"

#include <Logging.h>

#include <Arduino.h>
#include <pngle.h>
#include <tjpgd.h>

#include <cstdlib>
#include <cstring>

namespace ts {
namespace {

constexpr uint32_t kBandRows = 16;
constexpr uint16_t kMaxJpegWidth = 3072;  // 16-row band stays within 48 KB
constexpr uint16_t kMaxPngWidth = 8192;

struct Heap {
  void* p[12]{};
  int n = 0;
  void* take(const size_t bytes) {
    if (bytes == 0 || n >= 12) {
      return nullptr;
    }
    void* b = malloc(bytes);
    if (b) {
      p[n++] = b;
    }
    return b;
  }
  ~Heap() {
    for (int i = 0; i < n; ++i) {
      free(p[i]);
    }
  }
};

struct PageSink {
  HalFile* file = nullptr;
  uint16_t rowBytes = 0;
  uint16_t offX = 0;
  uint16_t fitY = 0;
  int16_t* errCur = nullptr;
  int16_t* errNext = nullptr;
  uint8_t* packed = nullptr;
  uint8_t* zeros = nullptr;
  uint16_t rows = 0;
  bool ok = true;
};

struct RowScaler {
  uint16_t srcW = 0;
  uint16_t srcH = 0;
  uint16_t dstW = 0;
  uint16_t dstH = 0;
  uint8_t* dstRow = nullptr;
  uint32_t* sums = nullptr;
  uint16_t* counts = nullptr;
  PageSink* sink = nullptr;
  uint32_t nextDst = 0;
  bool stopped = false;

  void resetAccum() {
    memset(sums, 0, static_cast<size_t>(dstW) * sizeof(uint32_t));
    memset(counts, 0, static_cast<size_t>(dstW) * sizeof(uint16_t));
  }

  bool emitAveraged() {
    for (uint32_t x = 0; x < dstW; ++x) {
      dstRow[x] = counts[x] != 0 ? static_cast<uint8_t>(sums[x] / counts[x]) : 0xFF;
    }
    if (!sink->ok) {
      stopped = true;
      return false;
    }
    if ((sink->rows & 15) == 0) {
      yield();
    }
    memset(sink->packed, 0, sink->rowBytes);
    ditherRow(dstRow, dstW, sink->errCur, sink->errNext, sink->packed, sink->offX);
    if (sink->file->write(sink->packed, sink->rowBytes) != sink->rowBytes) {
      sink->ok = false;
      stopped = true;
      return false;
    }
    ++sink->rows;
    ++nextDst;
    resetAccum();
    return true;
  }

  bool feedSourceRow(const uint32_t srcY, const uint8_t* gray) {
    while (nextDst < dstH && srcY >= static_cast<uint64_t>(nextDst + 1) * srcH / dstH) {
      if (!emitAveraged()) {
        return false;
      }
    }
    if (nextDst >= dstH || !gray) {
      return true;
    }
    for (uint32_t x = 0; x < srcW; ++x) {
      const uint32_t dx = static_cast<uint64_t>(x) * dstW / srcW;
      sums[dx] += gray[x];
      ++counts[dx];
    }
    if (srcY + 1 == srcH) {
      while (nextDst < dstH) {
        if (!emitAveraged()) {
          return false;
        }
      }
    }
    return true;
  }
};

bool allocScaler(RowScaler& scaler, Heap& heap) {
  scaler.dstRow = static_cast<uint8_t*>(heap.take(scaler.dstW));
  scaler.sums = static_cast<uint32_t*>(heap.take(static_cast<size_t>(scaler.dstW) * sizeof(uint32_t)));
  scaler.counts = static_cast<uint16_t*>(heap.take(static_cast<size_t>(scaler.dstW) * sizeof(uint16_t)));
  if (!scaler.dstRow || !scaler.sums || !scaler.counts) {
    return false;
  }
  scaler.resetAccum();
  return true;
}

// `packed` and `zeros` are already allocated (one page row each).
bool allocSink(PageSink& sink, Heap& heap, const uint16_t fitW) {
  sink.errCur = static_cast<int16_t*>(heap.take(static_cast<size_t>(fitW) * sizeof(int16_t)));
  sink.errNext = static_cast<int16_t*>(heap.take(static_cast<size_t>(fitW) * sizeof(int16_t)));
  if (!sink.errCur || !sink.errNext || !sink.packed || !sink.zeros) {
    return false;
  }
  memset(sink.errCur, 0, static_cast<size_t>(fitW) * sizeof(int16_t));
  return true;
}

bool writeZeros(HalFile& file, uint8_t* zeros, const uint16_t rowBytes, const uint16_t nrows) {
  for (uint16_t i = 0; i < nrows; ++i) {
    if (file.write(zeros, rowBytes) != rowBytes) {
      return false;
    }
  }
  return true;
}

struct JpegDec {
  HalFile* file = nullptr;
  uint8_t* skip = nullptr;
  bool ioError = false;
  RowScaler scaler;
  uint8_t* band = nullptr;
  uint16_t bandW = 0;
  uint32_t bandTop = 0;
  uint32_t bandFilled = 0;

  void flush(const uint32_t upTo) {
    while (bandTop < upTo && !scaler.stopped) {
      scaler.feedSourceRow(bandTop, band + static_cast<size_t>(bandTop % kBandRows) * bandW);
      ++bandTop;
    }
  }
};

size_t jpegInput(JDEC* jd, uint8_t* buf, const size_t len) {
  auto* dec = static_cast<JpegDec*>(jd->device);
  size_t total = 0;
  while (total < len) {
    uint8_t* dst = buf != nullptr ? buf + total : dec->skip;
    const size_t want = buf != nullptr ? len - total : (len - total < 1024 ? len - total : 1024);
    const int n = dec->file->read(dst, want);
    if (n < 0) {
      dec->ioError = true;
      return 0;
    }
    if (n == 0) {
      break;
    }
    total += static_cast<size_t>(n);
  }
  return total;
}

int jpegOutput(JDEC* jd, void* bitmap, JRECT* rect) {
  auto* dec = static_cast<JpegDec*>(jd->device);
  if (dec->scaler.stopped) {
    return 0;
  }
  if (static_cast<uint32_t>(rect->top) >= dec->bandFilled) {
    dec->flush(dec->bandFilled);
    dec->bandFilled = static_cast<uint32_t>(rect->bottom) + 1;
  }
  const auto* src = static_cast<const uint8_t*>(bitmap);
  const uint16_t rw = static_cast<uint16_t>(rect->right - rect->left + 1);
  for (uint16_t y = rect->top; y <= rect->bottom; ++y) {
    memcpy(dec->band + (static_cast<size_t>(y % kBandRows) * dec->bandW) + rect->left, src, rw);
    src += rw;
  }
  return 1;
}

uint8_t rgbaToGray(const uint8_t rgba[4]) {
  const int gray = (rgba[0] * 299 + rgba[1] * 587 + rgba[2] * 114) / 1000;
  return static_cast<uint8_t>(255 - ((255 - gray) * rgba[3]) / 255);
}

struct PngDec {
  Heap* heap = nullptr;
  PageSink* sink = nullptr;
  RowScaler scaler;
  uint8_t* srcRow = nullptr;
  uint16_t pageW = 0;
  uint16_t pageH = 0;
  uint32_t currentY = 0;
  bool rowOpen = false;
  bool unsupported = false;
  bool failed = false;
  bool ready = false;
};

void pngDraw(pngle_t* png, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  auto* dec = static_cast<PngDec*>(pngle_get_user_data(png));
  if (!dec || dec->unsupported || dec->failed || !dec->ready || dec->scaler.stopped) {
    return;
  }
  const uint8_t gray = rgbaToGray(rgba);
  for (uint32_t yy = y; yy < y + h; ++yy) {
    if (yy != dec->currentY || !dec->rowOpen) {
      if (dec->rowOpen && yy < dec->currentY) {
        dec->failed = true;
        return;
      }
      if (dec->rowOpen && !dec->scaler.feedSourceRow(dec->currentY, dec->srcRow)) {
        return;
      }
      const uint32_t gap0 = dec->rowOpen ? dec->currentY + 1 : 0;
      for (uint32_t gap = gap0; gap < yy; ++gap) {
        memset(dec->srcRow, 0xFF, dec->scaler.srcW);
        if (!dec->scaler.feedSourceRow(gap, dec->srcRow)) {
          return;
        }
      }
      memset(dec->srcRow, 0xFF, dec->scaler.srcW);
      dec->currentY = yy;
    }
    for (uint32_t xx = x; xx < x + w && xx < dec->scaler.srcW; ++xx) {
      dec->srcRow[xx] = gray;
    }
    dec->rowOpen = true;
  }
}

void pngOnInit(pngle_t* png, uint32_t w, uint32_t h) {
  auto* dec = static_cast<PngDec*>(pngle_get_user_data(png));
  const pngle_ihdr_t* ihdr = pngle_get_ihdr(png);
  if (!dec || !ihdr || ihdr->interlace != 0) {
    if (dec) {
      dec->unsupported = true;
    }
    return;
  }
  if (w == 0 || h == 0 || w > kMaxPngWidth || h > 65535) {
    dec->failed = true;
    return;
  }
  const FitBox fit = containFit(static_cast<uint16_t>(w), static_cast<uint16_t>(h), dec->pageW, dec->pageH);
  if (fit.w == 0 || fit.h == 0 || !allocSink(*dec->sink, *dec->heap, fit.w)) {
    dec->failed = true;
    return;
  }
  dec->sink->offX = fit.x;
  dec->sink->fitY = fit.y;
  dec->scaler.srcW = static_cast<uint16_t>(w);
  dec->scaler.srcH = static_cast<uint16_t>(h);
  dec->scaler.dstW = fit.w;
  dec->scaler.dstH = fit.h;
  dec->scaler.sink = dec->sink;
  dec->srcRow = static_cast<uint8_t*>(dec->heap->take(w));
  if (!dec->srcRow || !allocScaler(dec->scaler, *dec->heap)) {
    dec->failed = true;
    return;
  }
  memset(dec->srcRow, 0xFF, w);
  if (!writeZeros(*dec->sink->file, dec->sink->zeros, dec->sink->rowBytes, fit.y)) {
    dec->failed = true;
    return;
  }
  dec->ready = true;
}

struct PngHold {
  pngle_t* p = nullptr;
  ~PngHold() {
    if (p) {
      pngle_destroy(p);
    }
  }
};

}  // namespace

bool PictureWriter::begin(const char* path, const uint16_t w, const uint16_t h) {
  end();
  if (!path || w < 4 || h == 0 || (w % 4) != 0) {
    return false;
  }
  pageW = w;
  pageH = h;
  rowBytes = static_cast<uint16_t>(w / 4);
  pageBytes = static_cast<uint32_t>(rowBytes) * h;
  n = 0;
  if (!Storage.openFileForWrite("PIC", path, out)) {
    pageW = 0;
    return false;
  }
  const uint32_t magic = kPictureMagic;
  const uint16_t zero = 0;
  const uint32_t pad = 0;
  if (out.write(&magic, 4) != 4 || out.write(&n, 2) != 2 || out.write(&pageW, 2) != 2 || out.write(&pageH, 2) != 2 ||
      out.write(&zero, 2) != 2 || out.write(&pad, 4) != 4) {
    end();
    return false;
  }
  return true;
}

void PictureWriter::end() {
  if (out.isOpen()) {
    out.close();
  }
  pageW = 0;
  pageH = 0;
  n = 0;
  rowBytes = 0;
  pageBytes = 0;
}

bool PictureWriter::seekSlot() { return out.seekSet(kPictureHeader + static_cast<uint32_t>(n) * pageBytes); }

bool PictureWriter::writeZeroRows(uint8_t* zeros, const uint16_t nrows) {
  return writeZeros(out, zeros, rowBytes, nrows);
}

uint16_t PictureWriter::commit() {
  if (n >= kMaxPictures) {
    return 0;
  }
  const uint16_t id = static_cast<uint16_t>(n + 1);
  if (!out.seekSet(4) || out.write(&id, 2) != 2) {
    return 0;
  }
  n = id;
  return id;
}

uint16_t PictureWriter::addJpeg(HalFile& image) {
  if (!ready() || n >= kMaxPictures) {
    return 0;
  }
  Heap heap;
  auto* work = static_cast<uint8_t*>(heap.take(8 * 1024));
  auto* skip = static_cast<uint8_t*>(heap.take(1024));
  if (!work || !skip || !image.seekSet(0)) {
    return 0;
  }
  JpegDec dec;
  dec.file = &image;
  dec.skip = skip;
  JDEC jd;
  const JRESULT prep = jd_prepare(&jd, jpegInput, work, 8 * 1024, &dec);
  if (prep != JDR_OK) {
    LOG_INF("PIC", "jpeg prepare %d", static_cast<int>(prep));
    return 0;
  }
  uint8_t scale = 0;
  while (scale < 3 && (jd.width >> (scale + 1)) >= pageW && (jd.height >> (scale + 1)) >= pageH) {
    ++scale;
  }
  const uint16_t srcW = static_cast<uint16_t>(jd.width >> scale);
  const uint16_t srcH = static_cast<uint16_t>(jd.height >> scale);
  if (srcW == 0 || srcH == 0 || srcW > kMaxJpegWidth) {
    LOG_INF("PIC", "jpeg skip %ux%u scale %u", jd.width, jd.height, scale);
    return 0;
  }
  const FitBox fit = containFit(srcW, srcH, pageW, pageH);
  if (fit.w == 0 || fit.h == 0) {
    return 0;
  }
  PageSink sink;
  sink.file = &out;
  sink.rowBytes = rowBytes;
  sink.offX = fit.x;
  sink.fitY = fit.y;
  sink.packed = static_cast<uint8_t*>(heap.take(rowBytes));
  sink.zeros = static_cast<uint8_t*>(heap.take(rowBytes));
  if (!sink.packed || !sink.zeros) {
    return 0;
  }
  memset(sink.zeros, 0, rowBytes);
  if (!allocSink(sink, heap, fit.w)) {
    return 0;
  }
  dec.scaler.srcW = srcW;
  dec.scaler.srcH = srcH;
  dec.scaler.dstW = fit.w;
  dec.scaler.dstH = fit.h;
  dec.scaler.sink = &sink;
  dec.bandW = srcW;
  dec.band = static_cast<uint8_t*>(heap.take(static_cast<size_t>(srcW) * kBandRows));
  if (!dec.band || !allocScaler(dec.scaler, heap) || !seekSlot() || !writeZeroRows(sink.zeros, fit.y)) {
    return 0;
  }
  const JRESULT jr = jd_decomp(&jd, jpegOutput, scale);
  if (!dec.scaler.stopped) {
    dec.flush(srcH);
  }
  if (dec.ioError || (jr != JDR_OK && !dec.scaler.stopped) || !sink.ok || sink.rows != fit.h) {
    LOG_INF("PIC", "jpeg decode %d rows %u/%u", static_cast<int>(jr), sink.rows, fit.h);
    return 0;
  }
  if (!writeZeroRows(sink.zeros, static_cast<uint16_t>(pageH - fit.y - fit.h))) {
    return 0;
  }
  return commit();
}

uint16_t PictureWriter::addPng(HalFile& image) {
  if (!ready() || n >= kMaxPictures) {
    return 0;
  }
  Heap heap;
  PageSink sink;
  sink.file = &out;
  sink.rowBytes = rowBytes;
  sink.packed = static_cast<uint8_t*>(heap.take(rowBytes));
  sink.zeros = static_cast<uint8_t*>(heap.take(rowBytes));
  auto* window = static_cast<uint8_t*>(heap.take(8192));
  if (!sink.packed || !sink.zeros || !window || !image.seekSet(0)) {
    return 0;
  }
  memset(sink.zeros, 0, rowBytes);
  PngDec dec;
  dec.heap = &heap;
  dec.sink = &sink;
  dec.pageW = pageW;
  dec.pageH = pageH;
  PngHold hold;
  hold.p = pngle_new();
  if (!hold.p || !seekSlot()) {
    return 0;
  }
  pngle_set_user_data(hold.p, &dec);
  pngle_set_init_callback(hold.p, pngOnInit);
  pngle_set_draw_callback(hold.p, pngDraw);
  size_t have = 0;
  bool io = false;
  while (!dec.failed && !dec.unsupported && !dec.scaler.stopped) {
    if (have >= 8192) {
      dec.failed = true;
      break;
    }
    const int nread = image.read(window + have, 8192 - have);
    if (nread < 0) {
      io = true;
      break;
    }
    if (nread == 0) {
      break;
    }
    have += static_cast<size_t>(nread);
    const int ate = pngle_feed(hold.p, window, have);
    if (ate < 0) {
      dec.failed = true;
      break;
    }
    if (ate > 0) {
      memmove(window, window + ate, have - static_cast<size_t>(ate));
      have -= static_cast<size_t>(ate);
    }
  }
  if (dec.ready && dec.rowOpen && !dec.failed) {
    dec.scaler.feedSourceRow(dec.currentY, dec.srcRow);
  }
  if (io || dec.failed || dec.unsupported || !dec.ready || !sink.ok || sink.rows != dec.scaler.dstH) {
    LOG_INF("PIC", "png skip interlace=%d rows=%u", dec.unsupported ? 1 : 0, sink.rows);
    return 0;
  }
  const uint16_t bottom = static_cast<uint16_t>(pageH - sink.fitY - dec.scaler.dstH);
  if (!writeZeroRows(sink.zeros, bottom)) {
    return 0;
  }
  return commit();
}

bool pictureRead(const char* path, const uint16_t id, const uint16_t pageW, const uint16_t pageH, uint8_t* dst,
                 const size_t dstBytes) {
  if (!path || !dst || id == 0 || pageW < 4 || (pageW % 4) != 0 || pageH == 0) {
    return false;
  }
  const size_t need = static_cast<size_t>(pageW / 4) * pageH;
  if (dstBytes < need) {
    return false;
  }
  HalFile f;
  if (!Storage.openFileForRead("PIC", path, f)) {
    return false;
  }
  uint8_t hdr[16];
  if (f.read(hdr, 16) != 16) {
    return false;
  }
  uint32_t magic = 0;
  uint16_t count = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  memcpy(&magic, hdr, 4);
  memcpy(&count, hdr + 4, 2);
  memcpy(&w, hdr + 6, 2);
  memcpy(&h, hdr + 8, 2);
  if (magic != kPictureMagic || w != pageW || h != pageH || id > count) {
    return false;
  }
  const uint32_t off = kPictureHeader + static_cast<uint32_t>(id - 1) * static_cast<uint32_t>(need);
  if (!f.seekSet(off)) {
    return false;
  }
  return f.read(dst, need) == static_cast<int>(need);
}

}  // namespace ts
