#include "Layout.h"

#include "Kinsoku.h"

#include <algorithm>

namespace ts {
namespace {

void copyRuby(GlyphRun& run, const uint32_t* ruby, const uint8_t rubyCount) {
  const uint8_t n = rubyCount > 4 ? 4 : rubyCount;
  run.rubyCount = n;
  for (uint8_t i = 0; i < n; ++i) {
    run.ruby[i] = ruby ? ruby[i] : 0;
  }
}

}  // namespace

void PageLayouter::begin(const LayoutOptions& options) {
  opt = options;
  if (opt.em < 1) {
    opt.em = 1;
  }
  currentCount = 0;
  committedCount = 0;
  pageReady = false;
  lastAtomPos = 0;
  firstOnCurrent = 0;
  feedPos = 0;
  columnClosed = false;
  if (!recomputeGrid()) {
    return;
  }
  resetCursor();
}

bool PageLayouter::recomputeGrid() {
  rubyGutter = opt.compactColumns && !opt.hasRuby ? static_cast<int16_t>(std::max(1, opt.em / 5))
                                                  : static_cast<int16_t>((opt.em + 1) / 2);
  pitch = static_cast<int16_t>(opt.em + rubyGutter);
  const int16_t innerW = static_cast<int16_t>(opt.width - opt.margin * 2);
  const int16_t innerH = static_cast<int16_t>(opt.height - opt.margin * 2);

  if (opt.mode == WritingMode::VerticalRl) {
    const int nCols = pitch > 0 ? innerW / pitch : 0;
    const int nChars = opt.em > 0 ? innerH / opt.em : 0;
    if (nCols < 1 || nChars < 1) {
      return false;
    }
    const int16_t usedW = static_cast<int16_t>(nCols * pitch);
    const int16_t usedH = static_cast<int16_t>(nChars * opt.em);
    const int16_t padLeft = static_cast<int16_t>((innerW - usedW) / 2);
    const int16_t padTop = static_cast<int16_t>((innerH - usedH) / 2);
    colTop = static_cast<int16_t>(opt.margin + padTop);
    colBottom = static_cast<int16_t>(colTop + usedH);
    hanmenLeft = static_cast<int16_t>(opt.margin + padLeft);
    firstColX = static_cast<int16_t>(hanmenLeft + usedW - pitch);
  } else {
    const int nChars = opt.em > 0 ? innerW / opt.em : 0;
    const int nLines = pitch > 0 ? innerH / pitch : 0;
    if (nChars < 1 || nLines < 1) {
      return false;
    }
    const int16_t usedW = static_cast<int16_t>(nChars * opt.em);
    const int16_t usedH = static_cast<int16_t>(nLines * pitch);
    const int16_t padLeft = static_cast<int16_t>((innerW - usedW) / 2);
    const int16_t padTop = static_cast<int16_t>((innerH - usedH) / 2);
    firstLineY = static_cast<int16_t>(opt.margin + padTop);
    hanmenLeft = static_cast<int16_t>(opt.margin + padLeft);
    lineRight = static_cast<int16_t>(hanmenLeft + usedW);
    hanmenBottom = static_cast<int16_t>(firstLineY + usedH);
  }
  return true;
}

void PageLayouter::resetCursor() {
  columnClosed = false;
  if (opt.mode == WritingMode::VerticalRl) {
    indent = colTop;
    colX = firstColX;
    y = indent;
  } else {
    indent = hanmenLeft;
    lineY = firstLineY;
    x = indent;
  }
}

void PageLayouter::emit(const GlyphRun& run, const uint32_t pos) {
  if (currentCount >= kMaxGlyphs) {
    newPage();
  }
  if (currentCount == 0) {
    firstOnCurrent = pos;
  }
  if (currentCount < kMaxGlyphs) {
    current[currentCount++] = run;
  }
}

void PageLayouter::newPage() {
  committedCount = currentCount;
  for (uint16_t i = 0; i < currentCount; ++i) {
    committed[i] = current[i];
  }
  currentCount = 0;
  firstOnCurrent = 0;
  resetCursor();
  pageReady = true;
}

void PageLayouter::newColumn() {
  columnClosed = false;
  colX = static_cast<int16_t>(colX - pitch);
  y = indent;
  if (colX < hanmenLeft) {
    newPage();
  }
}

void PageLayouter::newLine() {
  columnClosed = false;
  lineY = static_cast<int16_t>(lineY + pitch);
  x = indent;
  if (lineY + pitch > hanmenBottom) {
    newPage();
  }
}

uint16_t PageLayouter::stickyTail(const bool vertical) const {
  if (currentCount == 0) {
    return 0;
  }
  const GlyphRun& last = current[currentCount - 1];
  if (vertical) {
    if (last.x != colX) {
      return 0;
    }
  } else if (last.y != glyphY()) {
    return 0;
  }
  uint16_t n = 1;
  while (n < currentCount && n < 8) {
    const GlyphRun& g = current[currentCount - n];
    if (!runSticky(g)) {
      break;
    }
    const GlyphRun& prev = current[currentCount - n - 1];
    if (vertical) {
      if (prev.x != colX) {
        break;
      }
    } else if (prev.y != glyphY()) {
      break;
    }
    ++n;
  }
  return n;
}

void PageLayouter::relocateTail(const uint16_t nIn, const bool vertical) {
  if (nIn == 0 || currentCount < nIn) {
    return;
  }
  const uint16_t n = nIn > 8 ? 8 : nIn;
  GlyphRun tail[8];
  const uint16_t base = static_cast<uint16_t>(currentCount - n);
  for (uint16_t i = 0; i < n; ++i) {
    tail[i] = current[base + i];
  }
  currentCount = base;
  if (currentCount == 0) {
    firstOnCurrent = 0;
  }
  if (vertical) {
    newColumn();
  } else {
    newLine();
  }
  const int16_t lead = tail[0].rubyLead;
  const int16_t firstAt = vertical ? tail[0].y : tail[0].x;
  const int16_t origin = static_cast<int16_t>(firstAt - lead);
  const int16_t lastEnd = static_cast<int16_t>((vertical ? tail[n - 1].y : tail[n - 1].x) + runAdvance(tail[n - 1]));
  const int16_t span = static_cast<int16_t>(lastEnd - origin + lead);
  const int16_t top = vertical ? y : x;
  for (uint16_t i = 0; i < n; ++i) {
    GlyphRun g = tail[i];
    const int16_t at = static_cast<int16_t>(top + ((vertical ? tail[i].y : tail[i].x) - origin));
    if (vertical) {
      g.x = colX;
      g.y = at;
    } else {
      g.x = at;
      g.y = glyphY();
    }
    emit(g, lastAtomPos);
  }
  if (vertical) {
    y = static_cast<int16_t>(top + span);
  } else {
    x = static_cast<int16_t>(top + span);
  }
}

void PageLayouter::placeChar(const uint32_t cp, const uint32_t* ruby, const uint8_t rubyCount, const bool tcy,
                             const uint8_t emphasis, const uint32_t* tcyCps, const uint8_t tcyN) {
  const int16_t F = opt.em;
  const int16_t h = kinsokuInlineAdvance(cp, F, tcy);
  const bool vert = opt.mode == WritingMode::VerticalRl;

  auto fill = [&](GlyphRun& run) {
    run.cp = cp;
    run.size = static_cast<uint16_t>(F);
    run.advance = static_cast<uint8_t>(h > 255 ? 255 : h);
    run.emphasis = emphasis;
    if (tcy && tcyCps && tcyN > 0) {
      copyRuby(run, tcyCps, tcyN);
      run.cp = tcyCps[0];
      run.flags = static_cast<uint8_t>(run.flags | kRunTcy);
    } else if (ruby && rubyCount > 0) {
      copyRuby(run, ruby, rubyCount);
    }
    if (h < F) {
      run.flags = static_cast<uint8_t>(run.flags | kRunHalfCell);
    }
  };

  if (vert) {
    if (columnClosed) {
      newColumn();
    }
    if (!tcy && kinsokuHangs(cp) && y > indent && y <= colBottom && static_cast<int>(y) + h > colBottom) {
      GlyphRun run{};
      fill(run);
      run.x = colX;
      run.y = y;
      if (shouldRotate(cp)) {
        run.flags = static_cast<uint8_t>(run.flags | kRunRotate);
      }
      emit(run, feedPos);
      columnClosed = true;
      return;
    }
    if (static_cast<int>(y) + h > colBottom) {
      const bool startKinsoku = !kinsokuCanStartColumn(cp);
      const bool endKinsoku = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
      const uint16_t tail = stickyTail(true);
      if ((startKinsoku || endKinsoku) && tail > 0) {
        relocateTail(tail, true);
      } else {
        newColumn();
      }
    }
    if (static_cast<int>(y) + h > colBottom) {
      newColumn();
    }
    GlyphRun run{};
    fill(run);
    run.x = colX;
    run.y = y;
    if (!tcy && shouldRotate(cp)) {
      run.flags = static_cast<uint8_t>(run.flags | kRunRotate);
    }
    emit(run, feedPos);
    y = static_cast<int16_t>(y + h);
    return;
  }

  if (columnClosed) {
    newLine();
  }
  if (!tcy && kinsokuHangs(cp) && x > indent && x <= lineRight && static_cast<int>(x) + h > lineRight) {
    GlyphRun run{};
    fill(run);
    run.x = x;
    run.y = glyphY();
    run.flags = static_cast<uint8_t>(run.flags | kRunRubyAbove);
    emit(run, feedPos);
    columnClosed = true;
    return;
  }
  if (static_cast<int>(x) + h > lineRight) {
    const bool startKinsoku = !kinsokuCanStartColumn(cp);
    const bool endKinsoku = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
    const uint16_t tail = stickyTail(false);
    if ((startKinsoku || endKinsoku) && tail > 0) {
      relocateTail(tail, false);
    } else {
      newLine();
    }
  }
  if (static_cast<int>(x) + h > lineRight) {
    newLine();
  }
  GlyphRun run{};
  fill(run);
  run.x = x;
  run.y = glyphY();
  run.flags = static_cast<uint8_t>(run.flags | kRunRubyAbove);
  emit(run, feedPos);
  x = static_cast<int16_t>(x + h);
}

void PageLayouter::placeGroup(const Atom& atom) {
  const int16_t F = opt.em;
  const bool vert = opt.mode == WritingMode::VerticalRl;
  const uint8_t nBase = atom.tcyCount > 4 ? 4 : atom.tcyCount;
  if (nBase == 0) {
    return;
  }
  const uint8_t nRuby = atom.rubyCount > 4 ? 4 : atom.rubyCount;
  int16_t advs[4]{};
  int16_t baseW = 0;
  for (uint8_t i = 0; i < nBase; ++i) {
    advs[i] = kinsokuInlineAdvance(atom.tcy[i], F, false);
    baseW = static_cast<int16_t>(baseW + advs[i]);
  }
  const int16_t rubyUnit = static_cast<int16_t>(F / 2);
  const int16_t rubyW = static_cast<int16_t>(nRuby * (rubyUnit > 0 ? rubyUnit : 1));
  const int16_t span = baseW > rubyW ? baseW : rubyW;
  const int16_t lead = static_cast<int16_t>((span - baseW) / 2);

  if (vert) {
    if (columnClosed) {
      newColumn();
    }
    if (static_cast<int>(y) + span > colBottom && y > indent) {
      const bool cannotEnd = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
      const uint16_t tail = stickyTail(true);
      if (cannotEnd && tail > 0) {
        relocateTail(tail, true);
      } else {
        newColumn();
      }
      if (static_cast<int>(y) + span > colBottom && y > indent) {
        newColumn();
      }
    }
  } else if (columnClosed) {
    newLine();
  }
  if (!vert && static_cast<int>(x) + span > lineRight && x > indent) {
    const bool cannotEnd = currentCount > 0 && !kinsokuCanEndColumn(current[currentCount - 1].cp);
    const uint16_t tail = stickyTail(false);
    if (cannotEnd && tail > 0) {
      relocateTail(tail, false);
    } else {
      newLine();
    }
    if (static_cast<int>(x) + span > lineRight && x > indent) {
      newLine();
    }
  }

  const int16_t top = vert ? y : x;
  int16_t at = static_cast<int16_t>(top + lead);
  for (uint8_t i = 0; i < nBase; ++i) {
    GlyphRun run{};
    run.cp = atom.tcy[i];
    run.size = static_cast<uint16_t>(F);
    run.advance = static_cast<uint8_t>(advs[i] > 255 ? 255 : advs[i]);
    run.emphasis = atom.emphasis;
    if (i == 0) {
      copyRuby(run, atom.ruby, nRuby);
      run.rubyLead = static_cast<uint8_t>(lead > 255 ? 255 : lead);
    } else {
      run.flags = static_cast<uint8_t>(run.flags | kRunSticky);
    }
    if (advs[i] < F) {
      run.flags = static_cast<uint8_t>(run.flags | kRunHalfCell);
    }
    if (!vert) {
      run.flags = static_cast<uint8_t>(run.flags | kRunRubyAbove);
    } else if (shouldRotate(run.cp)) {
      run.flags = static_cast<uint8_t>(run.flags | kRunRotate);
    }
    if (vert) {
      run.x = colX;
      run.y = at;
    } else {
      run.x = at;
      run.y = glyphY();
    }
    emit(run, feedPos);
    at = static_cast<int16_t>(at + advs[i]);
  }
  if (vert) {
    y = static_cast<int16_t>(top + span);
  } else {
    x = static_cast<int16_t>(top + span);
  }
}

bool PageLayouter::feed(const Atom& atom, const uint32_t atomPos) {
  pageReady = false;
  feedPos = atomPos;
  switch (atom.kind) {
    case AtomKind::PageBreak:
      if (currentCount > 0) {
        newPage();
      }
      resetCursor();
      if (opt.mode == WritingMode::VerticalRl) {
        indent = colTop;
        y = colTop;
      } else {
        indent = hanmenLeft;
        x = indent;
        lineY = firstLineY;
      }
      break;
    case AtomKind::Mode: {
      const WritingMode next = atom.cp == 1 ? WritingMode::HorizontalTb : WritingMode::VerticalRl;
      if (next != opt.mode) {
        if (currentCount > 0) {
          newPage();
        }
        opt.mode = next;
        if (recomputeGrid()) {
          resetCursor();
        }
      }
      break;
    }
    case AtomKind::ColumnBreak: {
      if (opt.mode == WritingMode::VerticalRl) {
        const int16_t next = std::min(static_cast<int16_t>(colTop + atom.startEm * opt.em),
                                      static_cast<int16_t>(colBottom - opt.em));
        if (y > indent) {
          indent = next;
          newColumn();
        } else {
          indent = next;
          y = indent;
        }
      } else {
        const int16_t next = std::min(static_cast<int16_t>(hanmenLeft + atom.startEm * opt.em),
                                      static_cast<int16_t>(lineRight - opt.em));
        if (x > indent) {
          indent = next;
          newLine();
        } else {
          indent = next;
          x = indent;
        }
      }
      break;
    }
    case AtomKind::Space: {
      const int16_t w = static_cast<int16_t>((opt.em * 35) / 100);
      if (opt.mode == WritingMode::VerticalRl) {
        if (columnClosed) {
          newColumn();
        }
        if (y == indent) {
          break;
        }
        if (static_cast<int>(y) + w + opt.em > colBottom) {
          newColumn();
        } else {
          y = static_cast<int16_t>(y + w);
        }
      } else {
        if (columnClosed) {
          newLine();
        }
        if (x <= indent) {
          break;
        }
        if (static_cast<int>(x) + w + opt.em > lineRight) {
          newLine();
        } else {
          x = static_cast<int16_t>(x + w);
        }
      }
      break;
    }
    case AtomKind::Gap:
      // 縦書き: 字下げ is already a fullwidth space in the text.
      if (opt.mode == WritingMode::HorizontalTb && x > indent) {
        newLine();
      }
      break;
    case AtomKind::Tcy:
      if (opt.mode == WritingMode::HorizontalTb) {
        const int need = opt.em * (atom.tcyCount > 0 ? atom.tcyCount : 1);
        if (static_cast<int>(x) + need > lineRight && x > indent) {
          newLine();
        }
        const uint8_t n = atom.tcyCount > 0 ? atom.tcyCount : 1;
        for (uint8_t i = 0; i < n; ++i) {
          placeChar(i == 0 ? atom.cp : atom.tcy[i], nullptr, 0, false, atom.emphasis, nullptr, 0);
        }
      } else {
        const uint8_t n = atom.tcyCount > 0 ? atom.tcyCount : 1;
        const uint32_t* cps = atom.tcyCount > 0 ? atom.tcy : &atom.cp;
        placeChar(atom.cp, nullptr, 0, true, atom.emphasis, cps, n);
      }
      lastAtomPos = atomPos;
      break;
    case AtomKind::Group:
      placeGroup(atom);
      lastAtomPos = atomPos;
      break;
    case AtomKind::Ch:
      placeChar(atom.cp, atom.ruby, atom.rubyCount, false, atom.emphasis, nullptr, 0);
      lastAtomPos = atomPos;
      break;
  }
  return pageReady;
}

bool PageLayouter::finish() {
  if (currentCount == 0) {
    return false;
  }
  newPage();
  return true;
}

RubyAlong placeRubyAlong(const int16_t origin, const int16_t em, const int16_t rubyEm, const uint8_t count,
                         const bool loEmpty, const bool hiEmpty) {
  RubyAlong out{};
  if (count == 0 || em <= 0 || rubyEm <= 0) {
    return out;
  }
  int lo = origin;
  int hi = origin + em;
  if (loEmpty) {
    lo -= em;
  }
  if (hiEmpty) {
    hi += em;
  }
  int pitch = rubyEm;
  int span = static_cast<int>(count) * pitch;
  const int room = hi - lo;
  if (span > room) {
    pitch = room / count;
    if (pitch < 1) {
      pitch = 1;
    }
    span = static_cast<int>(count) * pitch;
  }
  int start = origin + (em - span) / 2;
  if (start < lo) {
    start = lo;
  }
  if (start + span > hi) {
    start = hi - span;
  }
  out.start = static_cast<int16_t>(start);
  out.pitch = static_cast<int16_t>(pitch);
  return out;
}

}  // namespace ts
