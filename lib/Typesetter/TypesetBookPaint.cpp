#include "TypesetBook.h"

#include "PicturePage.h"

#include <Gfx.h>
#include <Logging.h>
#include <ScratchHeap.h>

#include <cstdio>
#include <cstring>

namespace {

uint32_t fallbackCp(const uint32_t cp) {
  if (cp >= 0x2460 && cp <= 0x2468) {
    return static_cast<uint32_t>('1' + (cp - 0x2460));  // ①–⑨
  }
  if (cp == 0x2469) {
    return static_cast<uint32_t>('0');  // ⑩
  }
  return cp;
}

// Picture sidecar only. Glyphs deliberately do not use this map: XgfFont
// leaves its gray planes unmarked so the UC8253 nudge cannot eat the stem.
// 0 paper, 1 light, 2 dark, 3 black. Light marks the new plane only. Dark
// marks both. Black and paper stay on the B/W frame.
bool planeKeep(const uint8_t v, const XgfFont::Plane plane) {
  if (plane == XgfFont::Plane::Ink) {
    return v >= 1;
  }
  if (plane == XgfFont::Plane::Lsb) {
    return v == 2;
  }
  return v == 1 || v == 2;
}

// Logical portrait pixels, same 90° clockwise map as Gfx::drawPixel.
void blitPicture(Gfx& gfx, const uint8_t* bits, const int pageW, const int pageH, const XgfFont::Plane plane) {
  uint8_t* fb = gfx.frameBuffer();
  if (!fb || !bits || pageW <= 0 || pageH <= 0 || (pageW % 4) != 0) {
    return;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  const int rowBytes = pageW / 4;
  for (int y = 0; y < pageH; ++y) {
    const int phyX = y;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const bool black = plane == XgfFont::Plane::Ink;
    const uint8_t bit = static_cast<uint8_t>(1u << (7 - (phyX & 7)));
    const int col = phyX >> 3;
    const uint8_t* row = bits + static_cast<size_t>(y) * static_cast<size_t>(rowBytes);
    for (int x = 0; x < pageW; ++x) {
      const uint8_t v = static_cast<uint8_t>((row[x >> 2] >> (6 - 2 * (x & 3))) & 3);
      if (!planeKeep(v, plane)) {
        continue;
      }
      const int phyY = ph - 1 - x;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      uint8_t* cell = &fb[static_cast<size_t>(phyY) * stride + static_cast<size_t>(col)];
      if (black) {
        *cell &= static_cast<uint8_t>(~bit);
      } else {
        *cell |= bit;
      }
    }
  }
}

// Black pixels in a logical square. Bit 0 is black, same map as Gfx::drawPixel.
uint16_t cellBlack(const Gfx& gfx, const int x, const int y, const int size) {
  const uint8_t* fb = gfx.frameBuffer();
  if (!fb || size <= 0) {
    return 0;
  }
  const int stride = gfx.stride();
  const int pw = gfx.fbWidth();
  const int ph = gfx.fbHeight();
  uint16_t n = 0;
  for (int ly = y; ly < y + size; ++ly) {
    const int phyX = ly;
    if (phyX < 0 || phyX >= pw) {
      continue;
    }
    const uint8_t bit = static_cast<uint8_t>(1u << (7 - (phyX & 7)));
    const int col = phyX >> 3;
    for (int lx = x; lx < x + size; ++lx) {
      const int phyY = ph - 1 - lx;
      if (phyY < 0 || phyY >= ph) {
        continue;
      }
      if ((fb[static_cast<size_t>(phyY) * static_cast<size_t>(stride) + static_cast<size_t>(col)] & bit) == 0) {
        ++n;
      }
    }
  }
  return n;
}

int writeUtf8(const uint32_t cp, char* out) {
  if (cp < 0x21 || cp == 0x7F) {
    out[0] = '_';
    return 1;
  }
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp <= 0xFFFF) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = '?';
  return 1;
}

}  // namespace

void TypesetBook::paint(Gfx& gfx, const XgfFont::Plane plane) {
  // Ink is black on white. Gray planes are white marks on black, which is
  // what the waveform reads as light and dark.
  gfx.clear(plane != XgfFont::Plane::Ink);
  if (loadedPicture != 0) {
    if (pictureBits) {
      blitPicture(gfx, pictureBits, layoutOpt.width, layoutOpt.height, plane);
    }
    return;
  }
  if (plane != XgfFont::Plane::Ink) {
    // Stems stay on the ink frame. Walking glyphs here would miss the cache
    // while a refresh owns SPI and store a 0xFF block for that character.
    return;
  }
  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();

  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    const bool vert = !ts::runRubyAbove(g);
    const int adv = ts::runAdvance(g);
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;

    if (tcyRun) {
      const int n = g.rubyCount;
      int size = em / n;
      if (size < 1) {
        size = 1;
      }
      const int y0 = g.y + (em - size) / 2;
      const int x0 = g.x + (em - size * n) / 2;
      for (int r = 0; r < n; ++r) {
        uint16_t id = font.glyphId(g.ruby[r]);
        if (id == 0xFFFF) {
          id = font.glyphId(fallbackCp(g.ruby[r]));
        }
        if (id != 0xFFFF) {
          font.blitBox(gfx, x0 + r * size, y0, id, size, plane);
        }
      }
    } else {
      int drawX = g.x;
      int drawY = g.y;
      if ((g.flags & ts::kRunHalfCell) != 0 && g.advance > 0 && g.advance < g.size) {
        const int shift = (static_cast<int>(g.size) - static_cast<int>(g.advance)) / 2;
        if (vert) {
          drawY -= shift;
        } else {
          drawX -= shift;
        }
      }
      uint16_t id = font.glyphId(g.cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(g.cp));
      }
      if (id != 0xFFFF) {
        font.blit(gfx, drawX, drawY, id, false, ts::runRotate90(g), plane);
      }
    }

    if (g.emphasis != 0 && em > 0) {
      const bool hasRuby = !ts::runTcy(g) && g.rubyCount > 0;
      int gap = em / (hasRuby ? 10 : 4);
      if (gap < 1) {
        gap = 1;
      }
      if (g.emphasis == 2) {
        int thick = em / 14;
        if (thick < 1) {
          thick = 1;
        }
        if (vert) {
          gfx.fillRect(g.x + em + gap, g.y, thick, adv, true);
        } else {
          gfx.fillRect(g.x, g.y - gap - thick, adv, thick, true);
        }
      } else {
        int rad = em / 10;
        if (rad < 1) {
          rad = 1;
        }
        const int cx = vert ? g.x + em + gap : g.x + adv / 2;
        const int cy = vert ? g.y + adv / 2 : g.y - gap;
        const int r2 = rad * rad;
        for (int dy = -rad; dy <= rad; ++dy) {
          for (int dx = -rad; dx <= rad; ++dx) {
            if (dx * dx + dy * dy <= r2) {
              gfx.drawPixel(cx + dx, cy + dy, true);
            }
          }
        }
      }
    }

    if (tcyRun || g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }

    const int lead = g.rubyLead;
    const int origin = (vert ? g.y : g.x) - lead;
    int lastEnd = (vert ? g.y : g.x) + adv;
    for (uint16_t j = static_cast<uint16_t>(i + 1); j < loadedCount; ++j) {
      const ts::GlyphRun& o = loadedGlyphs[j];
      if (!ts::runSticky(o)) {
        break;
      }
      if (vert) {
        if (o.x != g.x) {
          break;
        }
      } else if (o.y != g.y) {
        break;
      }
      lastEnd = (vert ? o.y : o.x) + ts::runAdvance(o);
    }
    const int span = lastEnd - origin + lead;
    if (span <= 0) {
      continue;
    }
    const int clusterLo = origin;
    const int clusterHi = origin + span;
    auto inside = [&](const ts::GlyphRun& o) -> bool {
      if (vert) {
        return o.x == g.x && o.y >= clusterLo && o.y < clusterHi;
      }
      return o.y == g.y && o.x >= clusterLo && o.x < clusterHi;
    };
    auto blocks = [&](const int dir) -> bool {
      const ts::GlyphRun* best = nullptr;
      int bestAbs = 0;
      for (uint16_t j = 0; j < loadedCount; ++j) {
        const ts::GlyphRun& o = loadedGlyphs[j];
        if (inside(o)) {
          continue;
        }
        if (vert) {
          if (o.x != g.x) {
            continue;
          }
        } else if (o.y != g.y) {
          continue;
        }
        const int d = (vert ? o.y : o.x) - origin;
        if ((dir < 0 && d >= 0) || (dir > 0 && d <= 0)) {
          continue;
        }
        const int ad = d < 0 ? -d : d;
        if (!best || ad < bestAbs) {
          best = &o;
          bestAbs = ad;
        }
      }
      return best && best->rubyCount > 0 && bestAbs <= span;
    };
    const ts::RubyAlong along = ts::placeRubyAlong(static_cast<int16_t>(origin), static_cast<int16_t>(span),
                                                   static_cast<int16_t>(rubyEm), g.rubyCount, !blocks(-1), !blocks(1));
    const int beside = g.emphasis ? em * 28 / 100 : 0;
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      uint16_t rid = font.glyphId(g.ruby[r]);
      if (rid == 0xFFFF) {
        rid = font.glyphId(fallbackCp(g.ruby[r]));
      }
      if (rid == 0xFFFF) {
        continue;
      }
      const int at = along.start + r * along.pitch;
      const int rx = vert ? g.x + em + beside : at;
      const int ry = vert ? at : g.y - rubyEm - beside;
      font.blit(gfx, rx, ry, rid, true, false, plane);
    }
  }
}

void TypesetBook::logPaintedGlyphs(Gfx& gfx, const uint32_t pageIndex) const {
  const XgfFont::CacheStats st = font.cacheStats();
  if (loadedPicture != 0) {
    LOG_INF("TS", "gly p%lu picture=%u", static_cast<unsigned long>(pageIndex + 1), loadedPicture);
    return;
  }
  LOG_INF("TS", "gly p%lu runs=%u em=%u body=%u/%u ruby=%u/%u hit=%lu miss=%lu ff=%lu blk=%lu",
          static_cast<unsigned long>(pageIndex + 1), loadedCount, font.emPx(), st.bodyUsed, st.bodyCap, st.rubyUsed,
          st.rubyCap, static_cast<unsigned long>(st.hits), static_cast<unsigned long>(st.misses),
          static_cast<unsigned long>(st.rejected), static_cast<unsigned long>(st.blocked));

  // One token per codepoint: char+cp id hash sourceInk/drawnBlack flags.
  // B = cached bitmap is mostly ink. D = the cell is much blacker than the
  // bitmap. M = not in the cache, so the cell was left blank. r = ruby.
  static uint32_t seen[160];
  uint16_t nSeen = 0;
  char line[160];
  size_t used = 0;
  auto fresh = [&](const uint32_t key) -> bool {
    for (uint16_t i = 0; i < nSeen; ++i) {
      if (seen[i] == key) {
        return false;
      }
    }
    if (nSeen < 160) {
      seen[nSeen++] = key;
    }
    return true;
  };
  auto flush = [&]() {
    if (used == 0) {
      return;
    }
    line[used] = '\0';
    LOG_INF("TS", "gly %s", line);
    used = 0;
  };
  auto add = [&](const char* tok) {
    const size_t n = strlen(tok);
    if (used != 0 && used + 1 + n >= sizeof(line)) {
      flush();
    }
    if (used != 0 && used + 1 < sizeof(line)) {
      line[used++] = ' ';
    }
    if (n >= sizeof(line)) {
      return;
    }
    memcpy(line + used, tok, n);
    used += n;
  };
  auto token = [&](const uint32_t cp, const uint16_t id, const bool ruby, const int x, const int y, const int size,
                   const uint8_t flags, const bool measure) {
    char utf[4];
    const int un = writeUtf8(cp, utf);
    XgfFont::SlotProbe pr;
    const bool ok = id != 0xFFFF && font.probe(id, ruby, pr);
    const uint16_t dst = measure ? cellBlack(gfx, x, y, size) : 0;
    char mark[4] = {};
    size_t m = 0;
    if (!ok) {
      mark[m++] = 'M';
    } else if (pr.area != 0 && static_cast<uint32_t>(pr.ink) * 100u / pr.area >= 55u) {
      mark[m++] = 'B';
    }
    if (measure && ok && dst > static_cast<uint16_t>(pr.ink + pr.ink / 3 + 40)) {
      mark[m++] = 'D';
    }
    char tok[72];
    if (measure) {
      snprintf(tok, sizeof(tok), "%s%.*s+%04lX/%04X/%08lX/%u/%u/%02X%s", ruby ? "r" : "", un, utf,
               static_cast<unsigned long>(cp), id, static_cast<unsigned long>(ok ? pr.hash : 0), ok ? pr.ink : 0, dst,
               flags, mark);
    } else {
      snprintf(tok, sizeof(tok), "%s%.*s+%04lX/%04X/%08lX/%u/-/%02X%s", ruby ? "r" : "", un, utf,
               static_cast<unsigned long>(cp), id, static_cast<unsigned long>(ok ? pr.hash : 0), ok ? pr.ink : 0, flags,
               mark);
    }
    add(tok);
  };

  const int em = font.emPx();
  const int rubyEm = font.rubyEmPx();
  for (uint16_t i = 0; i < loadedCount; ++i) {
    const ts::GlyphRun& g = loadedGlyphs[i];
    const bool vert = !ts::runRubyAbove(g);
    const bool tcyRun = ts::runTcy(g) && g.rubyCount > 1;
    if (tcyRun) {
      const int n = g.rubyCount;
      int size = em / n;
      if (size < 1) {
        size = 1;
      }
      const int y0 = g.y + (em - size) / 2;
      const int x0 = g.x + (em - size * n) / 2;
      for (int r = 0; r < n; ++r) {
        const uint32_t cp = g.ruby[r];
        if (!fresh(cp | 0x40000000u)) {
          continue;
        }
        uint16_t id = font.glyphId(cp);
        if (id == 0xFFFF) {
          id = font.glyphId(fallbackCp(cp));
        }
        token(cp, id, false, x0 + r * size, y0, size, g.flags, false);
      }
      continue;
    }

    int drawX = g.x;
    int drawY = g.y;
    if ((g.flags & ts::kRunHalfCell) != 0 && g.advance > 0 && g.advance < g.size) {
      const int shift = (static_cast<int>(g.size) - static_cast<int>(g.advance)) / 2;
      if (vert) {
        drawY -= shift;
      } else {
        drawX -= shift;
      }
    }
    if (fresh(g.cp)) {
      uint16_t id = font.glyphId(g.cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(g.cp));
      }
      token(g.cp, id, false, drawX, drawY, em, g.flags, true);
    }
    if (g.rubyCount == 0 || rubyEm == 0) {
      continue;
    }
    for (uint8_t r = 0; r < g.rubyCount; ++r) {
      const uint32_t cp = g.ruby[r];
      if (!fresh(cp | 0x80000000u)) {
        continue;
      }
      uint16_t id = font.glyphId(cp);
      if (id == 0xFFFF) {
        id = font.glyphId(fallbackCp(cp));
      }
      token(cp, id, true, 0, 0, rubyEm, g.flags, false);
    }
  }
  flush();
}

bool TypesetBook::drawPage(Gfx& gfx, const uint32_t pageIndex, int& pagesUntilFullRefresh,
                          const int refreshFrequency) {
  if (!opened) {
    error = "closed";
    return false;
  }
  const unsigned long tDraw = millis();
  const XgfFont::CacheStats sd0 = font.cacheStats();
  const unsigned long tRebase = millis();
  flushPendingCleanup(gfx);
  const unsigned long rebaseMs = millis() - tRebase;
  const unsigned long tLay = millis();
  if (!layoutPage(pageIndex)) {
    return false;
  }
  const unsigned long layMs = millis() - tLay;
  // SD and the panel share SPI. Read the page before the grayscale waveform starts.
  const unsigned long tPic = millis();
  pictureBits = nullptr;
  if (loadedPicture != 0) {
    const uint16_t pw = static_cast<uint16_t>(layoutOpt.width);
    const uint16_t ph = static_cast<uint16_t>(layoutOpt.height);
    const size_t nbytes = static_cast<size_t>(pw / 4) * ph;
    uint8_t* dst = ScratchHeap::data();
    if (dst && ScratchHeap::size() >= nbytes && ts::pictureRead(picturePath, loadedPicture, pw, ph, dst, nbytes)) {
      pictureBits = dst;
    } else {
      LOG_ERR("TS", "picture %u missing", loadedPicture);
    }
  }
  const unsigned long picMs = millis() - tPic;
  const unsigned long tInk = millis();
  paint(gfx, XgfFont::Plane::Ink);
  const unsigned long inkMs = millis() - tInk;
  // After the displayed ink frame, before the refresh takes the SPI bus.
  const unsigned long tGly = millis();
  logPaintedGlyphs(gfx, pageIndex);
  const unsigned long glyMs = millis() - tGly;

  const unsigned long tBase = millis();
  if (pagesUntilFullRefresh <= 1) {
    if (gfx.combinesGrayscaleBase()) {
      gfx.startGrayscaleBase(HalDisplay::HALF_REFRESH);
    } else {
      gfx.present(HalDisplay::HALF_REFRESH);
      gfx.preconditionGrayscale();
    }
    pagesUntilFullRefresh = refreshFrequency;
  } else {
    gfx.startGrayscaleBase(HalDisplay::FAST_REFRESH);
    --pagesUntilFullRefresh;
  }
  const unsigned long baseFireMs = millis() - tBase;

  // The fast base refresh still owns SPI. Glyph gray is unmarked, and a miss
  // must not read the card (that 0xFF slot is the repeating block).
  font.setSdReads(false);
  const unsigned long tLsb = millis();
  paint(gfx, XgfFont::Plane::Lsb);
  const unsigned long lsbMs = millis() - tLsb;

  const unsigned long tBaseWait = millis();
  gfx.finishGrayscaleBase();
  const unsigned long baseWaitMs = millis() - tBaseWait;
  font.setSdReads(true);

  const unsigned long tLsbCopy = millis();
  gfx.copyGrayscaleLsbBuffers();
  const unsigned long lsbCopyMs = millis() - tLsbCopy;
  paint(gfx, XgfFont::Plane::Msb);
  const unsigned long tMsbCopy = millis();
  gfx.copyGrayscaleMsbBuffers();
  const unsigned long msbCopyMs = millis() - tMsbCopy;

  font.setSdReads(false);
  gfx.startGrayBuffer();
  const unsigned long tWait = millis();
  gfx.finishGrayBuffer();
  const unsigned long waitMs = millis() - tWait;
  font.setSdReads(true);

  // Cleanup image for the next page's differential. SPI is idle, so a miss
  // is a real glyph. Doing this before finishGrayBuffer cached the idle
  // 0xFF read, and that character stayed a block on every later page.
  const unsigned long tClean = millis();
  paint(gfx, XgfFont::Plane::Ink);
  const unsigned long cleanMs = millis() - tClean;
  const XgfFont::CacheStats sd1 = font.cacheStats();
  // Milliseconds except sd reads/bytes and the hit/miss counts. spi is the
  // base send, then the LSB copy, then the MSB copy. wave is the two BUSY waits.
  LOG_INF("TS",
          "perf p%lu lay %lu pic %lu ink %lu lsb %lu clean %lu gly %lu rebase %lu spi %lu/%lu/%lu wave %lu+%lu sd %lu/%lu/%lu "
          "hit+%lu miss+%lu body %u/%u total %lu",
          static_cast<unsigned long>(pageIndex + 1), layMs, picMs, inkMs, lsbMs, cleanMs, glyMs, rebaseMs, baseFireMs,
          lsbCopyMs, msbCopyMs, baseWaitMs, waitMs, static_cast<unsigned long>(sd1.sdReads - sd0.sdReads),
          static_cast<unsigned long>(sd1.sdBytes - sd0.sdBytes),
          static_cast<unsigned long>((sd1.sdUs - sd0.sdUs) / 1000u), static_cast<unsigned long>(sd1.hits - sd0.hits),
          static_cast<unsigned long>(sd1.misses - sd0.misses), sd1.bodyUsed, sd1.bodyCap, millis() - tDraw);
  cleanupPending = true;
  error = "";
  return true;
}

void TypesetBook::prefetchForward(const uint32_t fromPageIndex) {
  if (!opened || fromPageIndex + 1 >= nPages) {
    return;
  }
  const unsigned long t0 = millis();
  const XgfFont::CacheStats sd0 = font.cacheStats();
  (void)layoutPage(fromPageIndex + 1);
  const XgfFont::CacheStats sd1 = font.cacheStats();
  LOG_INF("TS", "perf prefetch p%lu %lums sd %lu/%lu/%lu miss+%lu", static_cast<unsigned long>(fromPageIndex + 2),
          millis() - t0, static_cast<unsigned long>(sd1.sdReads - sd0.sdReads),
          static_cast<unsigned long>(sd1.sdBytes - sd0.sdBytes),
          static_cast<unsigned long>((sd1.sdUs - sd0.sdUs) / 1000u),
          static_cast<unsigned long>(sd1.misses - sd0.misses));
}

void TypesetBook::flushPendingCleanup(Gfx& gfx) {
  if (!cleanupPending) {
    return;
  }
  gfx.cleanupGrayscaleBuffers();
  cleanupPending = false;
}
