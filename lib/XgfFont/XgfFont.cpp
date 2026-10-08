#include "XgfFont.h"

#include <EpdFontFamily.h>
#include <Gfx.h>
#include <Logging.h>
#include <Utf8.h>
#include <esp_heap_caps.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

uint8_t pixelAt(const uint8_t* slot, const uint8_t em, const int x, const int y) {
  if (x < 0 || y < 0 || x >= em || y >= em) {
    return 0;
  }
  const unsigned rowBytes = (static_cast<unsigned>(em) + 3u) / 4u;
  const uint8_t b = slot[static_cast<unsigned>(y) * rowBytes + static_cast<unsigned>(x) / 4u];
  const unsigned shift = 6u - (static_cast<unsigned>(x) % 4u) * 2u;
  return static_cast<uint8_t>((b >> shift) & 3u);
}

// 0 paper, 1 light, 2 dark, 3 black. Ink is every non-paper pixel, so the
// base frame is the full stroke. The UC8253 `_gc` nudge drives a marked
// gray pixel (BW 0x80 / WW 0x20) instead of holding the black the base
// just set. On a reading face those marks are the stem, not a photo gray,
// and the drive turns characters into blocks. Leave both gray planes
// unmarked so every glyph pixel holds. Picture pages use their own map.
bool planeKeep(const uint8_t v, const XgfFont::Plane plane) {
  return plane == XgfFont::Plane::Ink && v >= xgf::kInkThreshold;
}

// Paint `nBits` MSB-first bits from `src` into a panel row at pixel `x`.
// `black` clears those bits. Otherwise it sets them (white on a gray plane).
void paintRowBits(uint8_t* row, const int rowBits, int x, const uint8_t* src, int nBits, const bool black) {
  int s = 0;
  if (x < 0) {
    s = -x;
    nBits -= s;
    x = 0;
  }
  if (nBits <= 0 || x >= rowBits) {
    return;
  }
  if (nBits > rowBits - x) {
    nBits = rowBits - x;
  }
  while (nBits > 0) {
    const int dOff = x & 7;
    const int sOff = s & 7;
    int take = 8 - dOff;
    if (take > 8 - sOff) {
      take = 8 - sOff;
    }
    if (take > nBits) {
      take = nBits;
    }
    uint8_t chunk = static_cast<uint8_t>(src[s >> 3] << sOff);
    chunk = static_cast<uint8_t>(chunk >> dOff);
    const uint8_t m = static_cast<uint8_t>(((1u << take) - 1u) << (8 - dOff - take));
    const uint8_t bits = static_cast<uint8_t>(chunk & m);
    if (black) {
      row[x >> 3] &= static_cast<uint8_t>(~bits);
    } else {
      row[x >> 3] |= bits;
    }
    x += take;
    s += take;
    nBits -= take;
  }
}

void fillRowMask(uint8_t* mask, const uint8_t* src, const uint8_t em, const int rowBytes, const XgfFont::Plane plane) {
  memset(mask, 0, 8);
  int gx = 0;
  for (int b = 0; b < rowBytes && gx < em; ++b) {
    const uint8_t s = src[b];
    for (int n = 0; n < 4 && gx < em; ++n, ++gx) {
      const uint8_t v = static_cast<uint8_t>((s >> (6 - 2 * n)) & 3);
      if (planeKeep(v, plane)) {
        mask[gx >> 3] |= static_cast<uint8_t>(1u << (7 - (gx & 7)));
      }
    }
  }
}

}  // namespace

bool XgfFont::load(const char* path, const uint32_t lruMaxBytes, const bool wantRubyMap) {
  close();
  lruLimit = lruMaxBytes ? lruMaxBytes : kLruBytes;
  wantRuby = wantRubyMap;
  if (!path || path[0] == '\0') {
    error = "no font path";
    return false;
  }
  snprintf(filepath, sizeof(filepath), "%s", path);
  if (!Storage.openFileForRead("XGF", filepath, file)) {
    error = "font missing";
    LOG_ERR("XGF", "open %s failed", filepath);
    return false;
  }
  file.probeContiguous();
  if (!readHeader() || !loadTables()) {
    close();
    return false;
  }
  opened = true;
  error = "";
  LOG_INF("XGF", "Loaded %s em=%u ruby=%u body=%u rubySlots=%u iv=%u (lru later)", filepath, header.emPx,
          header.rubyEmPx, header.bodyCount, header.rubyCount, header.intervalCount);
  return true;
}

void XgfFont::close() {
  opened = false;
  releaseFile();
  freeTables();
  freeLru();
  header = {};
  filepath[0] = '\0';
}

void XgfFont::releaseFile() {
  if (file.isOpen()) {
    file.close();
  }
}

bool XgfFont::reopenFile() {
  if (file.isOpen()) {
    return true;
  }
  if (filepath[0] == '\0') {
    error = "font missing";
    return false;
  }
  if (!Storage.openFileForRead("XGF", filepath, file)) {
    error = "font missing";
    return false;
  }
  file.probeContiguous();
  return true;
}

bool XgfFont::readHeader() {
  if (!file.seekSet(0)) {
    error = "font seek";
    return false;
  }
  if (file.read(&header, sizeof(header)) != static_cast<int>(sizeof(header))) {
    error = "font header";
    return false;
  }
  if (header.magic != xgf::kMagic) {
    error = "bad font magic";
    return false;
  }
  if (header.version != xgf::kVersion) {
    error = "font version";
    return false;
  }
  if (header.bpp != 2 || header.emPx < 4 || header.emPx > 64) {
    error = "font geometry";
    return false;
  }
  if (header.bodyStride != xgf::slotStride(header.emPx)) {
    error = "body stride";
    return false;
  }
  if (header.rubyEmPx > 0 && header.rubyStride != xgf::slotStride(header.rubyEmPx)) {
    error = "ruby stride";
    return false;
  }
  if (header.intervalCount == 0 || header.intervalCount > xgf::kMaxIntervals || header.bodyCount == 0 ||
      header.bodyCount > xgf::kMaxGlyphs) {
    LOG_ERR("XGF", "counts body=%u iv=%u ruby=%u (max glyphs=%u iv=%u)", header.bodyCount, header.intervalCount,
            header.rubyCount, xgf::kMaxGlyphs, xgf::kMaxIntervals);
    error = "font counts";
    return false;
  }
  LOG_INF("XGF", "header em=%u rubyEm=%u body=%u iv=%u ruby=%u stride=%u", header.emPx, header.rubyEmPx,
          header.bodyCount, header.intervalCount, header.rubyCount, header.bodyStride);
  return true;
}

bool XgfFont::loadTables() {
  intervals = static_cast<xgf::Interval*>(malloc(sizeof(xgf::Interval) * header.intervalCount));
  if (!intervals) {
    error = "cmap oom";
    return false;
  }
  if (!file.seekSet(header.intervalsOff)) {
    error = "cmap seek";
    return false;
  }
  const size_t ivBytes = sizeof(xgf::Interval) * header.intervalCount;
  if (file.read(intervals, ivBytes) != static_cast<int>(ivBytes)) {
    error = "cmap read";
    return false;
  }
  uint32_t covered = 0;
  for (uint16_t i = 0; i < header.intervalCount; ++i) {
    const xgf::Interval& iv = intervals[i];
    if (iv.first > iv.last || (i > 0 && iv.first <= intervals[i - 1].last)) {
      error = "cmap order";
      return false;
    }
    const uint32_t span = static_cast<uint32_t>(iv.last - iv.first) + 1;
    if (static_cast<uint32_t>(iv.glyphId) + span > header.bodyCount) {
      error = "cmap ids";
      return false;
    }
    covered += span;
  }
  if (covered != header.bodyCount) {
    error = "cmap coverage";
    return false;
  }

  if (wantRuby && header.rubyCount > 0) {
    rubyMap = static_cast<uint16_t*>(malloc(sizeof(uint16_t) * header.rubyCount));
    if (!rubyMap) {
      error = "ruby map oom";
      return false;
    }
    if (!file.seekSet(header.rubyMapOff)) {
      error = "ruby map seek";
      return false;
    }
    const size_t rb = sizeof(uint16_t) * header.rubyCount;
    if (file.read(rubyMap, rb) != static_cast<int>(rb)) {
      error = "ruby map read";
      return false;
    }
  }
  buildHi();
  return true;
}

void XgfFont::buildHi() {
  memset(hi, 0, sizeof(hi));
  for (uint16_t i = 0; i < header.intervalCount; ++i) {
    const uint16_t b0 = static_cast<uint16_t>(intervals[i].first >> 8);
    const uint16_t b1 = static_cast<uint16_t>(intervals[i].last >> 8);
    for (uint16_t b = b0; b <= b1; ++b) {
      if (hi[b].count == 0) {
        hi[b].begin = i;
      }
      hi[b].count = static_cast<uint16_t>(i - hi[b].begin + 1);
    }
  }
}

void XgfFont::freeTables() {
  free(intervals);
  intervals = nullptr;
  free(rubyMap);
  rubyMap = nullptr;
}

void XgfFont::releaseMaps() { freeTables(); }

bool XgfFont::restoreMaps() { return loadTables(); }

bool XgfFont::allocTable(Lru& t, const uint16_t cap, const uint16_t stride) {
  if (cap == 0 || stride == 0) {
    return false;
  }
  t.pixels = static_cast<uint8_t*>(malloc(static_cast<size_t>(cap) * stride));
  t.entries = static_cast<LruEntry*>(malloc(sizeof(LruEntry) * cap));
  if (!t.pixels || !t.entries) {
    freeTable(t);
    return false;
  }
  for (uint16_t i = 0; i < cap; ++i) {
    t.entries[i] = LruEntry{};
  }
  t.cap = cap;
  t.used = 0;
  t.clock = 1;
  t.stride = stride;
  return true;
}

void XgfFont::freeTable(Lru& t) {
  free(t.pixels);
  t.pixels = nullptr;
  free(t.entries);
  t.entries = nullptr;
  t.cap = 0;
  t.used = 0;
  t.clock = 1;
  t.stride = 0;
}

bool XgfFont::allocLru() {
  if (bodyLru.pixels) {
    return true;
  }
  if (wantRuby && header.rubyStride > 0 && header.rubyCount > 0) {
    uint16_t cap = static_cast<uint16_t>(kRubyLruBytes / header.rubyStride);
    if (cap > header.rubyCount) {
      cap = header.rubyCount;
    }
    if (cap == 0) {
      cap = 1;
    }
    if (!allocTable(rubyLru, cap, header.rubyStride) && !allocTable(rubyLru, 1, header.rubyStride)) {
      LOG_ERR("XGF", "ruby lru oom stride=%u", header.rubyStride);
    }
  }

  const uint16_t bodyStride = header.bodyStride;
  if (bodyStride == 0) {
    error = "stride";
    return false;
  }
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t slack = 12 * 1024;
  size_t budget = largest > slack ? largest - slack : 0;
  if (budget > lruLimit) {
    budget = lruLimit;
  }
  uint16_t cap = static_cast<uint16_t>(budget / bodyStride);
  if (cap == 0 && largest >= static_cast<size_t>(bodyStride) + 2048) {
    cap = 1;
  }
  if (cap == 0 || !allocTable(bodyLru, cap, bodyStride)) {
    LOG_ERR("XGF", "body lru oom cap=%u stride=%u largest=%u", cap, bodyStride, static_cast<unsigned>(largest));
    error = "lru oom";
    freeLru();
    return false;
  }
  if (cap < 8) {
    LOG_INF("XGF", "body lru small cap=%u stride=%u largest=%u", cap, bodyStride, static_cast<unsigned>(largest));
  }
  LOG_INF("XGF", "LRU body %u x %u (%u bytes) ruby %u x %u (%u bytes)", bodyLru.cap, bodyLru.stride,
          static_cast<unsigned>(bodyLru.cap * bodyLru.stride), rubyLru.cap, rubyLru.stride,
          static_cast<unsigned>(rubyLru.cap * rubyLru.stride));
  return true;
}

void XgfFont::freeLru() {
  freeTable(bodyLru);
  freeTable(rubyLru);
}

uint16_t XgfFont::glyphId(const uint32_t cp) const {
  if (!intervals || cp > 0xFFFF) {
    return 0xFFFF;
  }
  const uint16_t b = static_cast<uint16_t>(cp >> 8);
  const uint16_t begin = hi[b].begin;
  const uint16_t n = hi[b].count;
  if (n == 0) {
    return 0xFFFF;
  }
  uint16_t lo = begin;
  uint16_t hiIx = static_cast<uint16_t>(begin + n);
  while (lo < hiIx) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hiIx - lo) / 2);
    const xgf::Interval& iv = intervals[mid];
    if (cp < iv.first) {
      hiIx = mid;
    } else if (cp > iv.last) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      return static_cast<uint16_t>(iv.glyphId + (cp - iv.first));
    }
  }
  return 0xFFFF;
}

uint16_t XgfFont::rubySlot(const uint16_t bodyId) const {
  if (!rubyMap || header.rubyCount == 0) {
    return 0xFFFF;
  }
  uint16_t lo = 0;
  uint16_t hiIx = header.rubyCount;
  while (lo < hiIx) {
    const uint16_t mid = static_cast<uint16_t>(lo + (hiIx - lo) / 2);
    if (rubyMap[mid] < bodyId) {
      lo = static_cast<uint16_t>(mid + 1);
    } else {
      hiIx = mid;
    }
  }
  if (lo < header.rubyCount && rubyMap[lo] == bodyId) {
    return lo;
  }
  return 0xFFFF;
}

bool XgfFont::readSlot(const uint16_t bodyId, const bool ruby, uint8_t* dest) {
  uint32_t off = 0;
  uint16_t stride = 0;
  if (ruby) {
    const uint16_t slot = rubySlot(bodyId);
    if (slot == 0xFFFF) {
      return false;
    }
    stride = header.rubyStride;
    off = header.rubyBitsOff + static_cast<uint32_t>(slot) * stride;
  } else {
    if (bodyId >= header.bodyCount) {
      return false;
    }
    stride = header.bodyStride;
    off = header.bodyBitsOff + static_cast<uint32_t>(bodyId) * stride;
  }
  const unsigned long tSd = micros();
  const bool sought = file.seekSet(off);
  const int n = sought ? file.read(dest, stride) : 0;
  statSdUs += static_cast<uint32_t>(micros() - tSd);
  ++statSdReads;
  if (n > 0) {
    statSdBytes += static_cast<uint32_t>(n);
  }
  if (!sought || n != static_cast<int>(stride)) {
    return false;
  }
  for (uint16_t i = 0; i < stride; ++i) {
    if (dest[i] != 0xFF) {
      return true;
    }
  }
  // Paper is 0x00. Every pixel 0xFF is MISO idle while the panel holds the
  // bus, not a mincho slot (this face has none). Leave it uncached.
  ++statFf;
  LOG_ERR("XGF", "slot %u %s is 0xFF", bodyId, ruby ? "ruby" : "body");
  return false;
}

bool XgfFont::probe(const uint16_t bodyId, const bool ruby, SlotProbe& out) const {
  out = {};
  const Lru& t = ruby ? rubyLru : bodyLru;
  const uint8_t* slot = nullptr;
  for (uint16_t i = 0; i < t.used; ++i) {
    if (t.entries[i].id == bodyId) {
      slot = t.pixels + static_cast<size_t>(i) * t.stride;
      break;
    }
  }
  if (!slot || t.stride == 0) {
    return false;
  }
  out.cached = true;
  uint32_t hash = 2166136261u;
  for (uint16_t i = 0; i < t.stride; ++i) {
    hash ^= slot[i];
    hash *= 16777619u;
  }
  out.hash = hash;
  const uint8_t em = ruby ? header.rubyEmPx : header.emPx;
  out.area = static_cast<uint16_t>(em * em);
  uint16_t ink = 0;
  for (int y = 0; y < em; ++y) {
    for (int x = 0; x < em; ++x) {
      if (pixelAt(slot, em, x, y) >= xgf::kInkThreshold) {
        ++ink;
      }
    }
  }
  out.ink = ink;
  return true;
}

XgfFont::CacheStats XgfFont::cacheStats() const {
  CacheStats s;
  s.bodyUsed = bodyLru.used;
  s.bodyCap = bodyLru.cap;
  s.rubyUsed = rubyLru.used;
  s.rubyCap = rubyLru.cap;
  s.hits = statHit;
  s.misses = statMiss;
  s.rejected = statFf;
  s.blocked = statBlocked;
  s.sdReads = statSdReads;
  s.sdBytes = statSdBytes;
  s.sdUs = statSdUs;
  return s;
}

const uint8_t* XgfFont::cacheIn(Lru& t, const uint16_t bodyId, const bool ruby) {
  if (!t.pixels || t.cap == 0) {
    return nullptr;
  }
  uint16_t victim = 0;
  uint16_t oldest = 0xFFFF;
  for (uint16_t i = 0; i < t.used; ++i) {
    if (t.entries[i].id == bodyId) {
      t.entries[i].recency = ++t.clock;
      if (t.clock == 0xFFFF) {
        for (uint16_t j = 0; j < t.used; ++j) {
          t.entries[j].recency = static_cast<uint16_t>(t.entries[j].recency / 2);
        }
        t.clock = 0x8000;
      }
      ++statHit;
      return t.pixels + static_cast<size_t>(i) * t.stride;
    }
    if (t.entries[i].recency <= oldest) {
      oldest = t.entries[i].recency;
      victim = i;
    }
  }
  if (!allowSdReads) {
    ++statBlocked;
    return nullptr;
  }
  ++statMiss;
  uint16_t slot = victim;
  if (t.used < t.cap) {
    slot = t.used++;
  }
  uint8_t* dest = t.pixels + static_cast<size_t>(slot) * t.stride;
  if (!readSlot(bodyId, ruby, dest)) {
    t.entries[slot].id = 0xFFFF;
    return nullptr;
  }
  t.entries[slot].id = bodyId;
  t.entries[slot].recency = ++t.clock;
  return dest;
}

const uint8_t* XgfFont::cacheSlot(const uint16_t bodyId, const bool ruby) {
  if (!bodyLru.pixels && !allocLru()) {
    return nullptr;
  }
  return cacheIn(ruby ? rubyLru : bodyLru, bodyId, ruby);
}

void XgfFont::prewarm(const uint16_t* bodyIds, const uint16_t count, const bool ruby) {
  if (!bodyIds || count == 0 || !opened) {
    return;
  }
  const unsigned long t0 = millis();
  uint16_t hits = 0;
  uint16_t misses = 0;
  uint16_t order[256];
  uint16_t n = 0;
  for (uint16_t i = 0; i < count && n < 256; ++i) {
    const uint16_t id = bodyIds[i];
    bool dup = false;
    for (uint16_t j = 0; j < n; ++j) {
      if (order[j] == id) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      order[n++] = id;
    }
  }
  // Insertion sort by glyph id so SD reads run forward.
  for (uint16_t i = 1; i < n; ++i) {
    const uint16_t v = order[i];
    uint16_t j = i;
    while (j > 0 && order[j - 1] > v) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = v;
  }
  for (uint16_t i = 0; i < n; ++i) {
    if (ruby && rubySlot(order[i]) == 0xFFFF) {
      continue;
    }
    const Lru& table = ruby ? rubyLru : bodyLru;
    bool hit = false;
    for (uint16_t k = 0; k < table.used; ++k) {
      if (table.entries[k].id == order[i]) {
        hit = true;
        break;
      }
    }
    if (hit) {
      ++hits;
    } else {
      ++misses;
    }
    (void)cacheSlot(order[i], ruby);
  }
  LOG_INF("XGF", "prewarm %s n=%u hit=%u miss=%u %lums", ruby ? "ruby" : "body", n, hits, misses, millis() - t0);
}

bool XgfFont::blit(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const bool ruby, const bool rotate90,
                   const Plane plane) {
  const uint8_t* slot = cacheSlot(bodyId, ruby);
  if (!slot) {
    return false;
  }
  const uint8_t em = ruby ? header.rubyEmPx : header.emPx;
  if (em == 0) {
    return false;
  }
  uint8_t* fb = gfx.frameBuffer();
  if (!fb) {
    return false;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  const int rowBytes = (static_cast<int>(em) + 3) / 4;

  const bool black = plane == Plane::Ink;
  if (rotate90) {
    // Source row gy is consecutive phyX on panel row phyY (90° CW).
    uint8_t mask[8];
    for (int gy = 0; gy < em; ++gy) {
      const int phyY = ph - x - em + gy;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      fillRowMask(mask, slot + gy * rowBytes, em, rowBytes, plane);
      paintRowBits(fb + phyY * stride, pw, y, mask, em, black);
    }
    return true;
  }

  // Upright (ruby): source row is a panel column.
  for (int gy = 0; gy < em; ++gy) {
    const int phyX = y + gy;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const uint8_t clr = static_cast<uint8_t>(~(1u << (7 - (phyX & 7))));
    const int col = phyX >> 3;
    const uint8_t* src = slot + gy * rowBytes;
    int gx = 0;
    for (int b = 0; b < rowBytes && gx < em; ++b) {
      const uint8_t s = src[b];
      for (int n = 0; n < 4 && gx < em; ++n, ++gx) {
        const uint8_t v = static_cast<uint8_t>((s >> (6 - 2 * n)) & 3);
        if (!planeKeep(v, plane)) {
          continue;
        }
        const int phyY = ph - 1 - x - gx;
        if (phyY < 0 || phyY >= ph) {
          continue;
        }
        uint8_t* cell = &fb[phyY * stride + col];
        if (black) {
          *cell &= clr;
        } else {
          *cell |= static_cast<uint8_t>(~clr);
        }
      }
    }
  }
  return true;
}

bool XgfFont::blitBox(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const Plane plane) {
  if (size <= 0) {
    return false;
  }
  const uint8_t* slot = cacheSlot(bodyId, false);
  if (!slot) {
    return false;
  }
  const uint8_t em = header.emPx;
  if (em == 0) {
    return false;
  }
  if (size == static_cast<int>(em)) {
    return blit(gfx, x, y, bodyId, false, false, plane);
  }
  for (int dy = 0; dy < size; ++dy) {
    const int y0 = dy * static_cast<int>(em) / size;
    const int y1 = (dy + 1) * static_cast<int>(em) / size;
    for (int dx = 0; dx < size; ++dx) {
      const int x0 = dx * static_cast<int>(em) / size;
      const int x1 = (dx + 1) * static_cast<int>(em) / size;
      bool ink = false;
      for (int sy = y0; sy < y1 && !ink; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          if (planeKeep(pixelAt(slot, em, sx, sy), plane)) {
            ink = true;
            break;
          }
        }
      }
      if (ink) {
        gfx.drawPixel(x + dx, y + dy, plane == Plane::Ink);
      }
    }
  }
  return true;
}

bool XgfFont::blitUi(Gfx& gfx, const int x, const int y, const uint16_t bodyId, const int size, const bool black) {
  if (size <= 0) {
    return false;
  }
  const uint8_t* slot = cacheSlot(bodyId, false);
  if (!slot) {
    return false;
  }
  const uint8_t em = header.emPx;
  if (em == 0) {
    return false;
  }
  if (size == static_cast<int>(em)) {
    for (int gy = 0; gy < em; ++gy) {
      for (int gx = 0; gx < em; ++gx) {
        if (pixelAt(slot, em, gx, gy) >= xgf::kInkThreshold) {
          gfx.drawPixel(x + gx, y + gy, black);
        }
      }
    }
    return true;
  }
  for (int dy = 0; dy < size; ++dy) {
    const int y0 = dy * static_cast<int>(em) / size;
    const int y1 = (dy + 1) * static_cast<int>(em) / size;
    for (int dx = 0; dx < size; ++dx) {
      const int x0 = dx * static_cast<int>(em) / size;
      const int x1 = (dx + 1) * static_cast<int>(em) / size;
      bool ink = false;
      for (int sy = y0; sy < y1 && !ink; ++sy) {
        for (int sx = x0; sx < x1; ++sx) {
          if (pixelAt(slot, em, sx, sy) >= xgf::kInkThreshold) {
            ink = true;
            break;
          }
        }
      }
      if (ink) {
        gfx.drawPixel(x + dx, y + dy, black);
      }
    }
  }
  return true;
}

int XgfFont::drawUtf8(Gfx& gfx, const int fontId, const int x, const int y, const char* text, const bool black,
                      const int maxX) {
  if (!text || text[0] == '\0') {
    return 0;
  }
  if (!opened) {
    gfx.drawText(fontId, x, y, text, black);
    return gfx.textWidth(fontId, text);
  }
  const EpdFontFamily* family = gfx.font(fontId);
  const int lineH = family ? family->getData()->advanceY : 16;
  const int asc = family ? family->getData()->ascender : 12;
  int cell = asc > 0 ? asc : lineH;
  if (header.emPx > 0 && header.emPx < cell) {
    cell = header.emPx;
  }
  const int cjkY = y + (lineH - cell) / 2;
  int cursorX = x;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  uint32_t cp = 0;
  while ((cp = utf8NextCodepoint(&p))) {
    if (utf8IsCombiningMark(cp)) {
      continue;
    }
    if (family && family->hasCodepoint(cp)) {
      const int adv = gfx.drawCodepoint(fontId, cursorX, y, cp, black);
      if (adv <= 0) {
        continue;
      }
      if (maxX > 0 && cursorX + adv > maxX) {
        break;
      }
      cursorX += adv;
      continue;
    }
    const uint16_t id = glyphId(cp);
    if (id != 0xFFFF) {
      if (maxX > 0 && cursorX + cell > maxX) {
        break;
      }
      blitUi(gfx, cursorX, cjkY, id, cell, black);
      cursorX += cell;
      continue;
    }
    const int adv = gfx.drawCodepoint(fontId, cursorX, y, cp, black);
    if (adv <= 0) {
      continue;
    }
    if (maxX > 0 && cursorX + adv > maxX) {
      break;
    }
    cursorX += adv;
  }
  return cursorX - x;
}
