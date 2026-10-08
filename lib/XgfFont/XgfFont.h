#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "XgfFormat.h"

class Gfx;

class XgfFont {
 public:
  enum class Plane : uint8_t { Ink, Lsb, Msb };

  static constexpr uint32_t kLruBytes = 80 * 1024;
  static constexpr uint32_t kRubyLruBytes = 12 * 1024;
  static constexpr uint32_t kUiLruBytes = 16 * 1024;
  static constexpr const char* kDefaultPath = "/.crossjp/reading.xgf2";

  XgfFont() = default;
  ~XgfFont() { close(); }

  XgfFont(const XgfFont&) = delete;
  XgfFont& operator=(const XgfFont&) = delete;

  bool load(const char* path, uint32_t lruMaxBytes = kLruBytes, bool wantRuby = true);
  void close();
  bool loaded() const { return opened; }
  // Drop/restore cmap so EPUB ingest can use the heap. File stays open.
  void releaseMaps();
  bool restoreMaps();
  // Close the SD handle without dropping cmap or the path. Ingest reopens it.
  void releaseFile();
  bool reopenFile();

  uint8_t emPx() const { return header.emPx; }
  uint8_t rubyEmPx() const { return header.rubyEmPx; }
  uint16_t bodyCount() const { return header.bodyCount; }
  const char* lastError() const { return error; }

  // Unicode → glyph id. 0xFFFF = missing.
  uint16_t glyphId(uint32_t cp) const;
  bool hasGlyph(const uint32_t cp) const { return glyphId(cp) != 0xFFFF; }

  // Ruby slot for a body glyph id, or 0xFFFF.
  uint16_t rubySlot(uint16_t bodyId) const;

  // Load misses in glyphId order (coalesced SD). ids are body ids; ruby=true
  // fetches the ruby bitmap when present.
  void prewarm(const uint16_t* bodyIds, uint16_t count, bool ruby);

  // When false, a cache miss stays a miss and does not touch the SD. The panel
  // refresh shares SPI with the card; a read in that window comes back 0xFF
  // and would be reused as a black square for that glyph.
  void setSdReads(const bool allow) { allowSdReads = allow; }

  // Cached bitmap only. No SD read. hash is FNV-1a of the slot bytes.
  struct SlotProbe {
    uint16_t ink = 0;
    uint16_t area = 0;
    uint32_t hash = 0;
    bool cached = false;
  };
  bool probe(uint16_t bodyId, bool ruby, SlotProbe& out) const;

  struct CacheStats {
    uint16_t bodyUsed = 0;
    uint16_t bodyCap = 0;
    uint16_t rubyUsed = 0;
    uint16_t rubyCap = 0;
    uint32_t hits = 0;
    uint32_t misses = 0;
    uint32_t rejected = 0;  // all-0xFF slot, not cached
    uint32_t blocked = 0;   // miss while SD reads are suspended
    uint32_t sdReads = 0;
    uint32_t sdBytes = 0;
    uint32_t sdUs = 0;  // seek + read, microseconds
  };
  CacheStats cacheStats() const;

  // Blit one em (or ruby) slot at logical top-left. rotate90 is 90° CW, y-down.
  bool blit(Gfx& gfx, int x, int y, uint16_t bodyId, bool ruby, bool rotate90, Plane plane);

  // Scale a body glyph into a size×size box. Used for 縦中横. Honors the gray plane.
  bool blitBox(Gfx& gfx, int x, int y, uint16_t bodyId, int size, Plane plane);

  // 1-bit UI: Latin from Gfx fontId, other glyphs from this face, scaled to the
  // UI ascender. y matches Gfx::drawText. Stops before maxX (0 = no clip).
  int drawUtf8(Gfx& gfx, int fontId, int x, int y, const char* text, bool black, int maxX = 0);

 private:
  struct LruEntry {
    uint16_t id = 0xFFFF;  // 0xFFFF = empty
    uint16_t recency = 0;
  };
  struct Lru {
    uint8_t* pixels = nullptr;
    LruEntry* entries = nullptr;
    uint16_t cap = 0;
    uint16_t used = 0;
    uint16_t clock = 1;
    uint16_t stride = 0;
  };

  char filepath[256]{};
  const char* error = "not loaded";
  HalFile file;
  xgf::Header header{};
  bool opened = false;

  xgf::Interval* intervals = nullptr;
  uint16_t* rubyMap = nullptr;  // bodyId per ruby slot, sorted
  struct Hi {
    uint16_t begin = 0;
    uint16_t count = 0;
  };
  Hi hi[256]{};

  Lru bodyLru;
  Lru rubyLru;
  uint32_t lruLimit = kLruBytes;
  bool wantRuby = true;
  bool allowSdReads = true;
  uint32_t statHit = 0;
  uint32_t statMiss = 0;
  uint32_t statFf = 0;
  uint32_t statBlocked = 0;
  uint32_t statSdReads = 0;
  uint32_t statSdBytes = 0;
  uint32_t statSdUs = 0;

  bool readHeader();
  bool loadTables();
  void buildHi();
  void freeTables();
  bool allocLru();
  void freeLru();
  bool allocTable(Lru& t, uint16_t cap, uint16_t stride);
  void freeTable(Lru& t);
  const uint8_t* cacheIn(Lru& t, uint16_t bodyId, bool ruby);
  const uint8_t* cacheSlot(uint16_t bodyId, bool ruby);
  bool readSlot(uint16_t bodyId, bool ruby, uint8_t* dest);
  bool blitUi(Gfx& gfx, int x, int y, uint16_t bodyId, int size, bool black);
};
