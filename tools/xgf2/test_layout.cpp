// Host test for em-grid 縦書き + 禁則. Compile:
//   c++ -std=c++20 -I lib/Typesetter tools/xgf2/test_layout.cpp lib/Typesetter/Layout.cpp lib/Typesetter/Kinsoku.cpp -o /tmp/test_layout && /tmp/test_layout

#include "Kinsoku.h"
#include "Layout.h"
#include "PictureDither.h"

#include <cstdio>
#include <cstdlib>

static void fail(const char* msg) {
  std::fprintf(stderr, "FAIL %s\n", msg);
  std::exit(1);
}

int main() {
  if (ts::kinsokuCanStartColumn(0x3042) == false) {
    fail("あ should start a column");
  }
  if (ts::kinsokuCanStartColumn(0x3002) == true) {
    fail("。 must not start a column");
  }
  if (ts::kinsokuCanEndColumn(0x300C) == true) {
    fail("「 must not end a column");
  }
  if (!ts::shouldRotate('A') || ts::shouldRotate(0x4E00)) {
    fail("rotate ASCII not 一");
  }

  ts::LayoutOptions opt{};
  opt.width = 70;  // one column at em=20, ruby gutter 10, pitch 30 → 70/30 = 2 cols
  opt.height = 80;
  opt.em = 20;
  opt.margin = 0;
  opt.hasRuby = true;
  opt.mode = ts::WritingMode::VerticalRl;

  ts::PageLayouter lay;
  lay.begin(opt);

  int pages = 0;
  // 8 chars: 2 cols x 4 chars (80/20=4)
  for (int i = 0; i < 8; ++i) {
    ts::Atom a{};
    a.kind = ts::AtomKind::Ch;
    a.cp = 0x3042 + static_cast<uint32_t>(i % 5);
    if (lay.feed(a, static_cast<uint32_t>(i))) {
      ++pages;
      lay.clearPage();
    }
  }
  if (lay.finish()) {
    ++pages;
  }
  if (pages < 1) {
    fail("expected at least one page");
  }

  // 禁則: a column of 4 that would start the next with 。 should pull back.
  lay.begin(opt);
  pages = 0;
  const uint32_t seq[] = {0x3042, 0x3044, 0x3046, 0x3048, 0x3002};  // あいうえ。 wraps; 。 cannot start a column
  uint32_t seqPos = 0;
  for (uint32_t cp : seq) {
    ts::Atom a{};
    a.kind = ts::AtomKind::Ch;
    a.cp = cp;
    if (lay.feed(a, seqPos++)) {
      ++pages;
      lay.clearPage();
    }
  }
  lay.finish();
  bool foundKinsoku = false;
  for (uint16_t i = 0; i < lay.pageGlyphCount(); ++i) {
    if (lay.page()[i].cp == 0x3002) {
      foundKinsoku = true;
    }
  }
  if (!foundKinsoku && pages == 0) {
    fail("。 dropped");
  }

  std::printf("ok pages=%d glyphs=%u\n", pages, lay.pageGlyphCount());

  auto dump = [](const ts::PageLayouter& l) {
    for (uint16_t i = 0; i < l.pageGlyphCount(); ++i) {
      const ts::GlyphRun& g = l.page()[i];
      std::fprintf(stderr, "  [%u] U+%04X x=%d y=%d adv=%u flags=%u ruby=%u\n", i, g.cp, g.x, g.y, g.advance, g.flags,
                   g.rubyCount);
    }
  };
  auto must = [](ts::PageLayouter& l, const ts::Atom& a) {
    if (l.feed(a, 1)) {
      fail("unexpected page break");
    }
  };
  auto ch = [](uint32_t cp) {
    ts::Atom a{};
    a.kind = ts::AtomKind::Ch;
    a.cp = cp;
    return a;
  };

  // Vertical gap must not open a hole between あ and い.
  lay.begin(opt);
  must(lay, ch(0x3042));
  {
    ts::Atom gap{};
    gap.kind = ts::AtomKind::Gap;
    must(lay, gap);
  }
  must(lay, ch(0x3044));
  if (!lay.finish() || lay.pageGlyphCount() != 2) {
    dump(lay);
    fail("vertical gap glyph count");
  }
  if (lay.page()[0].x != lay.page()[1].x || lay.page()[1].y != lay.page()[0].y + opt.em) {
    dump(lay);
    fail("vertical gap separated あ and い");
  }

  // em=10, 10 chars per column, 2 columns. Half-em 。、 can hang in the pad.
  ts::LayoutOptions grid = opt;
  grid.width = 40;
  grid.height = 100;
  grid.em = 10;
  lay.begin(grid);
  must(lay, ch(0x3002));
  if (!lay.finish()) {
    fail("half advance page");
  }
  if (lay.page()[0].advance != 5 || (lay.page()[0].flags & ts::kRunHalfCell) == 0) {
    dump(lay);
    fail("。 advance");
  }
  lay.begin(grid);
  must(lay, ch(0x30FB));
  lay.finish();
  if (lay.page()[0].advance != 5 || (lay.page()[0].flags & ts::kRunHalfCell) == 0) {
    dump(lay);
    fail("・ advance");
  }
  lay.begin(grid);
  must(lay, ch(0x300C));
  lay.finish();
  if (lay.page()[0].advance != 10 || (lay.page()[0].flags & ts::kRunHalfCell) != 0) {
    dump(lay);
    fail("「 stays a full em");
  }

  const int col0 = lay.page()[0].x;
  const int top = lay.page()[0].y;
  const int col1 = col0 - 15;

  lay.begin(grid);
  for (int i = 0; i < 10; ++i) {
    must(lay, ch(0x3042));
  }
  must(lay, ch(0x3002));
  must(lay, ch(0x3044));
  if (!lay.finish()) {
    fail("hang page");
  }
  const ts::GlyphRun* period = nullptr;
  const ts::GlyphRun* nextCh = nullptr;
  for (uint16_t i = 0; i < lay.pageGlyphCount(); ++i) {
    if (lay.page()[i].cp == 0x3002) {
      period = &lay.page()[i];
    }
    if (lay.page()[i].cp == 0x3044) {
      nextCh = &lay.page()[i];
    }
  }
  if (!period || period->x != col0 || period->y != top + 100 || !nextCh || nextCh->x != col1 || nextCh->y != top) {
    dump(lay);
    fail("。 hang did not close the column");
  }

  lay.begin(grid);
  for (int i = 0; i < 10; ++i) {
    must(lay, ch(0x3042));
  }
  must(lay, ch(0x3002));
  must(lay, ch(0x3002));
  lay.finish();
  int periods = 0;
  bool secondAtTop = false;
  for (uint16_t i = 0; i < lay.pageGlyphCount(); ++i) {
    if (lay.page()[i].cp != 0x3002) {
      continue;
    }
    ++periods;
    if (lay.page()[i].x == col1 && lay.page()[i].y == top) {
      secondAtTop = true;
    }
  }
  if (periods != 2 || !secondAtTop) {
    dump(lay);
    fail("second 。 should start the next column");
  }

  lay.begin(grid);
  for (int i = 0; i < 9; ++i) {
    must(lay, ch(0x3042));
  }
  must(lay, ch(0x300C));
  must(lay, ch(0x3044));
  lay.finish();
  const ts::GlyphRun* bracket = nullptr;
  nextCh = nullptr;
  for (uint16_t i = 0; i < lay.pageGlyphCount(); ++i) {
    if (lay.page()[i].cp == 0x300C) {
      bracket = &lay.page()[i];
    }
    if (lay.page()[i].cp == 0x3044) {
      nextCh = &lay.page()[i];
    }
  }
  if (!bracket || !nextCh || bracket->x != col1 || bracket->y != top || nextCh->x != col1 || nextCh->y != top + 10) {
    dump(lay);
    fail("「 should be pulled with the next char");
  }

  lay.begin(grid);
  for (int i = 0; i < 9; ++i) {
    must(lay, ch(0x3042));
  }
  {
    ts::Atom g{};
    g.kind = ts::AtomKind::Group;
    g.cp = 0x9580;
    g.tcyCount = 2;
    g.tcy[0] = 0x9580;
    g.tcy[1] = 0x8107;
    g.rubyCount = 4;
    g.ruby[0] = 0x304B;
    g.ruby[1] = 0x3069;
    g.ruby[2] = 0x308F;
    g.ruby[3] = 0x304D;
    must(lay, g);
  }
  lay.finish();
  const ts::GlyphRun* base0 = nullptr;
  const ts::GlyphRun* base1 = nullptr;
  for (uint16_t i = 0; i < lay.pageGlyphCount(); ++i) {
    if (lay.page()[i].cp == 0x9580) {
      base0 = &lay.page()[i];
    }
    if (lay.page()[i].cp == 0x8107) {
      base1 = &lay.page()[i];
    }
  }
  if (!base0 || !base1 || base0->x != col1 || base1->x != col1 || base0->y != top || base1->y != top + 10 ||
      base0->rubyCount != 4 || (base1->flags & ts::kRunSticky) == 0) {
    dump(lay);
    fail("group ruby split across a column");
  }

  ts::LayoutOptions horiz = grid;
  horiz.mode = ts::WritingMode::HorizontalTb;
  horiz.width = 100;
  horiz.height = 40;
  lay.begin(horiz);
  for (int i = 0; i < 9; ++i) {
    must(lay, ch(0x3042));
  }
  must(lay, ch(0x300C));
  must(lay, ch(0x3044));
  lay.finish();
  bracket = nullptr;
  nextCh = nullptr;
  const ts::GlyphRun* first = lay.pageGlyphCount() ? &lay.page()[0] : nullptr;
  for (uint16_t i = 0; i < lay.pageGlyphCount(); ++i) {
    if (lay.page()[i].cp == 0x300C) {
      bracket = &lay.page()[i];
    }
    if (lay.page()[i].cp == 0x3044) {
      nextCh = &lay.page()[i];
    }
  }
  if (!first || !bracket || !nextCh || bracket->y == first->y || bracket->x != first->x || nextCh->y != bracket->y ||
      nextCh->x != bracket->x + 10 || (bracket->flags & ts::kRunRubyAbove) == 0) {
    dump(lay);
    fail("horizontal 「 should wrap with the next char");
  }

  lay.begin(opt);
  must(lay, ch(0x3042));
  {
    ts::Atom mode{};
    mode.kind = ts::AtomKind::Mode;
    mode.cp = 1;
    if (!lay.feed(mode, 1)) {
      fail("mode change should close the page");
    }
  }
  lay.clearPage();
  must(lay, ch(0x3044));
  lay.finish();
  if (lay.pageGlyphCount() != 1 || (lay.page()[0].flags & ts::kRunRubyAbove) == 0) {
    dump(lay);
    fail("mode atom did not switch to horizontal");
  }

  auto expectRuby = [](const ts::RubyAlong& got, int start, int pitch, const char* name) {
    if (got.start != start || got.pitch != pitch) {
      std::fprintf(stderr, "FAIL %s start=%d pitch=%d (want %d %d)\n", name, got.start, got.pitch, start, pitch);
      std::exit(1);
    }
  };
  // em=20 rubyEm=10 origin=100
  expectRuby(ts::placeRubyAlong(100, 20, 10, 2, true, true), 100, 10, "2 kana fills cell");
  expectRuby(ts::placeRubyAlong(100, 20, 10, 1, true, true), 105, 10, "1 kana centered");
  expectRuby(ts::placeRubyAlong(100, 20, 10, 4, true, true), 90, 10, "4 kana overhang both empty");
  expectRuby(ts::placeRubyAlong(100, 20, 10, 4, false, false), 100, 5, "4 kana clamp both ruby");
  expectRuby(ts::placeRubyAlong(100, 20, 10, 3, false, true), 100, 10, "3 kana shift into empty next");
  expectRuby(ts::placeRubyAlong(100, 20, 10, 3, true, false), 90, 10, "3 kana shift into empty prev");
  expectRuby(ts::placeRubyAlong(100, 20, 10, 4, false, true), 100, 10, "4 kana only next empty");

  lay.begin(opt);
  {
    ts::Atom pic{};
    pic.kind = ts::AtomKind::Picture;
    pic.cp = 3;
    if (!lay.feed(pic, 8) || lay.takenPicture() != 3 || lay.deferredPicture() != 0 || lay.pageGlyphCount() != 0) {
      fail("picture on an empty page");
    }
  }
  lay.begin(opt);
  {
    ts::Atom a{};
    a.kind = ts::AtomKind::Ch;
    a.cp = 0x3042;
    lay.feed(a, 4);
    ts::Atom pic{};
    pic.kind = ts::AtomKind::Picture;
    pic.cp = 4;
    if (!lay.feed(pic, 20) || lay.deferredPicture() != 4 || lay.takenPicture() != 0 || lay.pageGlyphCount() < 1) {
      fail("picture after glyphs should defer");
    }
    lay.clearPage();
    if (!lay.feed(pic, 20) || lay.takenPicture() != 4 || lay.deferredPicture() != 0 || lay.pageGlyphCount() != 0) {
      fail("replayed picture should be an empty page");
    }
  }

  {
    int16_t cur[4]{};
    int16_t next[4]{};
    uint8_t packed[1]{};
    const uint8_t gray[4] = {255, 160, 96, 0};
    ts::ditherRow(gray, 4, cur, next, packed, 0);
    if (packed[0] != 0x1B) {
      std::fprintf(stderr, "packed %02x\n", packed[0]);
      fail("dither bins");
    }
    const ts::FitBox small = ts::containFit(100, 100, 528, 792);
    const ts::FitBox wide = ts::containFit(1000, 100, 528, 792);
    const ts::FitBox cover = ts::containFit(1600, 2400, 528, 792);
    if (small.w != 100 || small.h != 100 || small.x != 214 || small.y != 346 || wide.w != 528 || wide.h != 52 ||
        wide.y != 370 || cover.w != 528 || cover.h != 792 || cover.x != 0 || cover.y != 0) {
      fail("contain fit");
    }
  }

  return 0;
}
