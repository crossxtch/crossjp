#include "HtmlIR.h"

#include "HtmlSax.h"

#include <Utf8.h>

#include <cstdio>
#include <cstring>

namespace ts {
namespace {

bool emitAtom(const AtomSink& sink, const Atom& a, const uint32_t pos) {
  if (!sink.emit) {
    return true;
  }
  return sink.emit(sink.ctx, a, pos);
}

bool eq(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }

bool isSkip(const char* name) {
  return eq(name, "script") || eq(name, "style") || eq(name, "head") || eq(name, "rp");
}

bool isPara(const char* name) { return eq(name, "p") || eq(name, "li") || eq(name, "tr"); }

bool isHeading(const char* name) { return name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0; }

int startEmFromClass(const HtmlSax& sax) {
  char cls[256];
  if (!sax.attr("class", cls, sizeof(cls))) {
    return -1;
  }
  const char* s = cls;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    auto take = [&](const char* prefix) -> int {
      const size_t pn = strlen(prefix);
      if (static_cast<size_t>(e - s) <= pn || memcmp(s, prefix, pn) != 0) {
        return -1;
      }
      const char* n = s + pn;
      const char* ne = e;
      if (ne - n >= 2 && ne[-2] == 'e' && ne[-1] == 'm') {
        ne -= 2;
      }
      int v = 0;
      if (n == ne) {
        return -1;
      }
      while (n < ne) {
        if (*n < '0' || *n > '9') {
          return -1;
        }
        v = v * 10 + (*n - '0');
        ++n;
      }
      if (v >= 0 && v <= 20) {
        return v;
      }
      return -1;
    };
    int v = take("h-indent-");
    if (v < 0) {
      v = take("start-");
    }
    if (v >= 0) {
      return v;
    }
    s = e;
  }
  return -1;
}

WritingMode modeFromSax(const HtmlSax& sax) {
  if (sax.hasClass("hltr") || sax.hasClassPrefix("hltr") || sax.hasClass("horizontal-tb")) {
    return WritingMode::HorizontalTb;
  }
  if (sax.hasClass("vrtl") || sax.hasClassPrefix("vrtl") || sax.hasClass("vertical-rl")) {
    return WritingMode::VerticalRl;
  }
  char style[192];
  if (sax.attr("style", style, sizeof(style))) {
    if (strstr(style, "horizontal-tb") || strstr(style, "lr-tb") || strstr(style, "rl-tb")) {
      return WritingMode::HorizontalTb;
    }
    if (strstr(style, "vertical-rl") || strstr(style, "tb-rl") || strstr(style, "vertical-lr")) {
      return WritingMode::VerticalRl;
    }
  }
  return WritingMode::VerticalRl;
}

bool modeSpecified(const HtmlSax& sax) {
  return sax.hasClass("hltr") || sax.hasClassPrefix("hltr") || sax.hasClass("horizontal-tb") || sax.hasClass("vrtl") ||
         sax.hasClassPrefix("vrtl") || sax.hasClass("vertical-rl") || sax.attrContains("style", "writing-mode") ||
         sax.attrContains("style", "horizontal-tb") || sax.attrContains("style", "vertical-rl");
}

void appendUtf8(uint32_t* cps, uint8_t& n, const uint8_t cap, const char* s, const uint16_t slen) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  const unsigned char* end = p + slen;
  while (p < end && n < cap) {
    const unsigned char* next = p;
    const uint32_t cp = utf8NextCodepoint(&next);
    if (next == p) {
      break;
    }
    p = next;
    if (cp == 0) {
      break;
    }
    cps[n++] = cp;
  }
}

bool isWsCp(const uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0; }

Atom chAtom(const uint32_t cp, const uint32_t* ruby, const uint8_t rubyN, const uint8_t emphasis) {
  Atom a{};
  a.kind = AtomKind::Ch;
  a.cp = cp;
  a.rubyCount = rubyN > 8 ? 8 : rubyN;
  for (uint8_t i = 0; i < a.rubyCount; ++i) {
    a.ruby[i] = ruby[i];
  }
  a.emphasis = emphasis;
  return a;
}

bool isPlaceholder(const uint32_t cp) {
  switch (cp) {
    case 0x3013:  // 〓
    case 0xFFFD:
    case 0x25A0:  // ■
    case 0x25A1:  // □
    case 0x25CF:  // ●
    case 0x25CB:  // ○
    case 0x25C6:  // ◆
    case 0x25C7:  // ◇
    case 0x203B:  // ※
    case 0xFF0A:  // ＊
    case '*':
    case 0x2605:  // ★
    case 0x2606:  // ☆
    case 0xFF1F:  // ？
    case '?':
    case 0x25CE:  // ◎
      return true;
    default:
      return false;
  }
}

uint32_t cidChar(const int cid) {
  switch (cid) {
    case 13803:
      return 0x20B9F;  // 𠮟
    case 13706:
      return 0x20BB7;  // 𠮷
    case 20315:
      return 0x29E3D;  // 𩸽
    default:
      return 0;
  }
}

int cidFromSrc(const char* src) {
  if (!src) {
    return 0;
  }
  for (const char* p = src; *p; ++p) {
    const char c0 = p[0] | 0x20;
    const char c1 = p[1] ? static_cast<char>(p[1] | 0x20) : 0;
    const char c2 = p[2] ? static_cast<char>(p[2] | 0x20) : 0;
    if (c0 == 'c' && c1 == 'i' && c2 == 'd') {
      const char* n = p + 3;
      if (*n == '-' || *n == '_') {
        ++n;
      }
      if (*n < '0' || *n > '9') {
        continue;
      }
      int v = 0;
      while (*n >= '0' && *n <= '9') {
        v = v * 10 + (*n - '0');
        ++n;
      }
      return v;
    }
  }
  return 0;
}

uint8_t emphasisOf(const HtmlSax& sax) {
  if (sax.hasClass("em-line") || sax.hasClassPrefix("em-line")) {
    return 2;
  }
  if (sax.hasClass("em-sesame") || sax.hasClass("em-sesame-open") || sax.hasClass("em-dot") ||
      sax.hasClass("em-dot-open") || sax.hasClass("em-circle") || sax.hasClass("em-circle-open") ||
      sax.hasClass("em-double-circle") || sax.hasClass("em-double-circle-open") || sax.hasClass("em-triangle") ||
      sax.hasClass("em") || eq(sax.name, "em")) {
    return 1;
  }
  return 0;
}

int heightEmOf(const HtmlSax& sax) {
  char cls[256];
  if (!sax.attr("class", cls, sizeof(cls))) {
    return -1;
  }
  const char* s = cls;
  while (*s) {
    while (*s == ' ') {
      ++s;
    }
    const char* e = s;
    while (*e && *e != ' ') {
      ++e;
    }
    const char* prefix = "height-";
    const size_t pn = 7;
    if (static_cast<size_t>(e - s) > pn && memcmp(s, prefix, pn) == 0) {
      int v = 0;
      const char* n = s + pn;
      if (n < e && *n >= '0' && *n <= '9') {
        while (n < e && *n >= '0' && *n <= '9') {
          v = v * 10 + (*n - '0');
          ++n;
        }
        if (v > 0 && v <= 8) {
          return v;
        }
      }
    }
    s = e;
  }
  return -1;
}

bool sliceHas(const char* s, const size_t n, const char* lit) {
  const size_t m = strlen(lit);
  if (m == 0 || n < m) {
    return false;
  }
  for (size_t i = 0; i + m <= n; ++i) {
    if (memcmp(s + i, lit, m) == 0) {
      return true;
    }
  }
  return false;
}

WritingMode modeInSlice(const char* s, const size_t n, bool& found) {
  WritingMode mode = WritingMode::VerticalRl;
  if (sliceHas(s, n, "hltr") || sliceHas(s, n, "horizontal-tb") || sliceHas(s, n, "lr-tb") ||
      sliceHas(s, n, "rl-tb")) {
    mode = WritingMode::HorizontalTb;
    found = true;
  }
  if (sliceHas(s, n, "vrtl") || sliceHas(s, n, "vertical-rl") || sliceHas(s, n, "vertical-lr") ||
      sliceHas(s, n, "tb-rl")) {
    mode = WritingMode::VerticalRl;
    found = true;
  }
  return mode;
}

}  // namespace

void splitReading(const uint32_t* rt, const uint8_t rtN, const uint8_t baseN, uint32_t* out, uint8_t* outCount) {
  for (uint8_t i = 0; i < baseN; ++i) {
    outCount[i] = 0;
  }
  if (baseN == 0) {
    return;
  }
  if (rtN == baseN) {
    for (uint8_t i = 0; i < baseN; ++i) {
      out[i * 4] = rt[i];
      outCount[i] = 1;
    }
    return;
  }
  const uint8_t n = rtN > 4 ? 4 : rtN;
  outCount[0] = n;
  for (uint8_t i = 0; i < n; ++i) {
    out[i] = rt[i];
  }
}

static bool htmlFromSax(HtmlSax& sax, const AtomSink sink, HtmlIRResult* result);

bool sniffWritingMode(const char* data, const size_t len, WritingMode& mode) {
  if (!data || len == 0) {
    return false;
  }
  const size_t cap = len < 16384 ? len : 16384;
  bool cssFound = false;
  WritingMode cssMode = WritingMode::VerticalRl;
  size_t i = 0;
  while (i + 5 < cap) {
    if (data[i] != '<') {
      ++i;
      continue;
    }
    if (i + 6 <= cap && memcmp(data + i, "<style", 6) == 0) {
      size_t j = i + 6;
      while (j + 8 <= cap && memcmp(data + j, "</style>", 8) != 0) {
        ++j;
      }
      bool found = false;
      const WritingMode m = modeInSlice(data + i, j - i, found);
      if (found) {
        cssFound = true;
        cssMode = m;
      }
      i = j;
      continue;
    }
    const bool html = i + 5 <= cap && memcmp(data + i, "<html", 5) == 0;
    const bool body = i + 5 <= cap && memcmp(data + i, "<body", 5) == 0;
    if (html || body) {
      size_t j = i;
      while (j < cap && data[j] != '>') {
        ++j;
      }
      bool found = false;
      const WritingMode m = modeInSlice(data + i, j - i, found);
      if (found) {
        mode = m;
        return true;
      }
      i = j;
      continue;
    }
    ++i;
  }
  if (cssFound) {
    mode = cssMode;
    return true;
  }
  return false;
}

bool htmlToAtoms(const char* data, const size_t len, const AtomSink sink, HtmlIRResult* result) {
  HtmlSax sax;
  sax.bind(data, len);
  return htmlFromSax(sax, sink, result);
}

bool htmlToAtomsPull(int (*read)(void*, char*, int), void* ctx, const AtomSink sink, HtmlIRResult* result) {
  HtmlSax sax;
  sax.bindPull(read, ctx);
  return htmlFromSax(sax, sink, result);
}

static bool htmlFromSax(HtmlSax& sax, const AtomSink sink, HtmlIRResult* result) {
  HtmlIRResult local{};
  HtmlIRResult& res = result ? *result : local;

  bool tocLike = false;
  bool inBody = false;
  int skipDepth = 0;
  int rubyDepth = 0;
  int tcyDepth = 0;
  int indent = 0;
  int indentStack[16]{};
  uint8_t indentTop = 0;
  uint32_t tcyCps[4]{};
  uint8_t tcyN = 0;
  uint32_t rubyBase[8]{};
  uint8_t rubyBaseN = 0;
  uint32_t rubyRt[8]{};
  uint8_t rubyRtN = 0;
  bool inRt = false;
  bool inRb = false;
  bool lastWasGap = true;
  bool lastWasCol = false;
  bool lastWasGlyph = false;
  uint32_t textCount = 0;
  char titleBuf[96]{};
  bool capturingTitle = false;
  uint8_t titleN = 0;

  auto emit = [&](const Atom& a, const uint32_t pos) -> bool {
    if (a.kind == AtomKind::Gap) {
      if (lastWasGap || lastWasCol) {
        return true;
      }
      lastWasGap = true;
      lastWasGlyph = false;
    } else if (a.kind == AtomKind::ColumnBreak) {
      lastWasCol = true;
      lastWasGap = false;
      lastWasGlyph = false;
    } else if (a.kind == AtomKind::Space) {
      lastWasGlyph = false;
    } else {
      lastWasGap = false;
      lastWasCol = false;
      lastWasGlyph = a.kind == AtomKind::Ch || a.kind == AtomKind::Tcy || a.kind == AtomKind::Group;
    }
    return emitAtom(sink, a, pos);
  };

  int depth = 0;
  struct EmphFrame {
    int depth;
    uint8_t prev;
  };
  EmphFrame emphFrames[8]{};
  uint8_t emphN = 0;
  uint8_t curEmph = 0;
  bool pictureBody = false;
  bool inHeading = false;

  auto flushRubyPair = [&](const uint32_t pos) {
    if (rubyBaseN == 0) {
      rubyRtN = 0;
      return;
    }
    if (rubyBaseN == 1) {
      emit(chAtom(rubyBase[0], rubyRt, rubyRtN, curEmph), pos);
    } else if (rubyRtN != rubyBaseN) {
      Atom g{};
      g.kind = AtomKind::Group;
      g.tcyCount = rubyBaseN > 4 ? 4 : rubyBaseN;
      for (uint8_t i = 0; i < g.tcyCount; ++i) {
        g.tcy[i] = rubyBase[i];
      }
      g.cp = g.tcy[0];
      g.rubyCount = rubyRtN > 8 ? 8 : rubyRtN;
      for (uint8_t i = 0; i < g.rubyCount; ++i) {
        g.ruby[i] = rubyRt[i];
      }
      g.emphasis = curEmph;
      emit(g, pos);
      for (uint8_t i = g.tcyCount; i < rubyBaseN; ++i) {
        emit(chAtom(rubyBase[i], nullptr, 0, curEmph), pos);
      }
    } else {
      uint32_t packed[8 * 4]{};
      uint8_t counts[8]{};
      const uint8_t bases = rubyBaseN > 8 ? 8 : rubyBaseN;
      splitReading(rubyRt, rubyRtN, bases, packed, counts);
      for (uint8_t i = 0; i < bases; ++i) {
        emit(chAtom(rubyBase[i], packed + i * 4, counts[i], curEmph), pos);
      }
    }
    rubyBaseN = 0;
    rubyRtN = 0;
  };

  auto emitText2 = [&](const char* s, const uint16_t n, const uint32_t pos) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    const unsigned char* end = p + n;
    while (p < end) {
      const unsigned char* next = p;
      const uint32_t cp = utf8NextCodepoint(&next);
      if (next == p) {
        break;
      }
      p = next;
      if (cp == 0 || cp == '\n' || cp == '\r' || cp == '\t') {
        continue;
      }
      if (skipDepth > 0) {
        continue;
      }
      if (rubyDepth > 0) {
        if (inRt) {
          if (rubyRtN < 8) {
            rubyRt[rubyRtN++] = cp;
          }
        } else if (!isWsCp(cp)) {
          if (rubyBaseN < 8) {
            rubyBase[rubyBaseN++] = cp;
          }
        }
        continue;
      }
      if (tcyDepth > 0) {
        if (!isWsCp(cp) && tcyN < 4) {
          tcyCps[tcyN++] = cp;
        }
        continue;
      }
      if (cp == ' ' || cp == 0xA0) {
        if (lastWasGlyph) {
          Atom sp{};
          sp.kind = AtomKind::Space;
          emit(sp, pos);
        }
        continue;
      }
      ++textCount;
      if (capturingTitle) {
        unsigned char tmp[4];
        int tn = 0;
        if (cp < 0x80) {
          tmp[tn++] = static_cast<unsigned char>(cp);
        } else if (cp < 0x800) {
          tmp[tn++] = static_cast<unsigned char>(0xC0 | (cp >> 6));
          tmp[tn++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
          tmp[tn++] = static_cast<unsigned char>(0xE0 | (cp >> 12));
          tmp[tn++] = static_cast<unsigned char>(0x80 | ((cp >> 6) & 0x3F));
          tmp[tn++] = static_cast<unsigned char>(0x80 | (cp & 0x3F));
        }
        if (titleN + tn < sizeof(titleBuf) - 1) {
          memcpy(titleBuf + titleN, tmp, static_cast<size_t>(tn));
          titleN = static_cast<uint8_t>(titleN + tn);
          titleBuf[titleN] = 0;
        }
      }
      emit(chAtom(cp, nullptr, 0, curEmph), pos);
    }
  };
  (void)appendUtf8;

  while (sax.next()) {
    if (sax.kind == HtmlSax::Kind::Text) {
      emitText2(sax.text, sax.ntext, sax.pos);
      continue;
    }
    if (sax.kind == HtmlSax::Kind::Start || sax.kind == HtmlSax::Kind::Empty) {
      if (sax.kind == HtmlSax::Kind::Start && !isSkip(sax.name) && skipDepth == 0) {
        ++depth;
        const uint8_t mark = emphasisOf(sax);
        if (mark && emphN < 8) {
          emphFrames[emphN].depth = depth;
          emphFrames[emphN].prev = curEmph;
          ++emphN;
          curEmph = mark;
        }
      }
      if (eq(sax.name, "html") || eq(sax.name, "body")) {
        if (modeSpecified(sax)) {
          res.mode = modeFromSax(sax);
          res.hasMode = true;
        }
        if (eq(sax.name, "body")) {
          inBody = true;
          if (sax.hasClass("p-toc") || sax.hasClass("toc") || sax.hasClassPrefix("p-toc")) {
            tocLike = true;
          }
          if (sax.hasClass("p-image") || sax.hasClass("p-cover") || sax.hasClassPrefix("p-tobira")) {
            pictureBody = true;
          }
        }
      }
      if (eq(sax.name, "nav") && (sax.attrContains("type", "toc") || sax.hasClass("toc"))) {
        tocLike = true;
      }
      char id[64];
      if (sax.attr("id", id, sizeof(id)) && id[0]) {
        // anchors ignored for layout
      }
      if (isSkip(sax.name)) {
        if (sax.kind == HtmlSax::Kind::Start) {
          ++skipDepth;
        }
        continue;
      }
      if (skipDepth > 0) {
        continue;
      }
      const int em = startEmFromClass(sax);
      if (sax.kind == HtmlSax::Kind::Start && indentTop < 15) {
        indentStack[indentTop++] = indent;
        if (em >= 0) {
          indent = em;
        }
      } else if (em >= 0) {
        indent = em;
      }

      if (eq(sax.name, "ruby")) {
        ++rubyDepth;
        rubyBaseN = 0;
        rubyRtN = 0;
        inRt = false;
        inRb = false;
      } else if (eq(sax.name, "rt")) {
        if (rubyDepth > 0 && rubyBaseN > 0 && rubyRtN > 0) {
          flushRubyPair(sax.pos);
        }
        inRt = true;
        rubyRtN = 0;
      } else if (eq(sax.name, "rb")) {
        inRb = true;
      } else if (eq(sax.name, "br")) {
        Atom g{};
        g.kind = AtomKind::Gap;
        emit(g, sax.pos);
      } else if (eq(sax.name, "span") &&
                 (sax.hasClass("tcy") || sax.hasClassPrefix("tcy") || sax.attrContains("style", "text-combine"))) {
        ++tcyDepth;
        tcyN = 0;
      } else if (isHeading(sax.name)) {
        if (titleBuf[0] == 0) {
          capturingTitle = true;
          titleN = 0;
        }
        inHeading = true;
        Atom c{};
        c.kind = AtomKind::ColumnBreak;
        c.startEm = static_cast<uint8_t>(indent);
        emit(c, sax.pos);
      } else if (isPara(sax.name) || eq(sax.name, "div")) {
        if (tocLike && textCount < 2000 && isPara(sax.name)) {
          Atom c{};
          c.kind = AtomKind::ColumnBreak;
          c.startEm = static_cast<uint8_t>(indent);
          emit(c, sax.pos);
        } else if (!lastWasGap && !lastWasCol) {
          Atom g{};
          g.kind = AtomKind::Gap;
          emit(g, sax.pos);
        }
      } else if (eq(sax.name, "img") || eq(sax.name, "image")) {
        char alt[64];
        alt[0] = 0;
        sax.attr("alt", alt, sizeof(alt));
        char src[96];
        src[0] = 0;
        if (!sax.attr("src", src, sizeof(src))) {
          sax.attr("href", src, sizeof(src));
        }
        const bool gaiji = sax.hasClass("gaiji") || sax.hasClassPrefix("gaiji");
        uint32_t cps[12]{};
        uint8_t nCp = 0;
        if (alt[0]) {
          const unsigned char* ap = reinterpret_cast<const unsigned char*>(alt);
          const unsigned char* end = ap + strlen(alt);
          while (ap < end && nCp < 12) {
            const unsigned char* next = ap;
            const uint32_t cp = utf8NextCodepoint(&next);
            if (next == ap || cp == 0) {
              break;
            }
            ap = next;
            cps[nCp++] = cp;
          }
        }
        const bool oneChar = nCp == 1 && !isPlaceholder(cps[0]);
        if (gaiji) {
          if (oneChar) {
            emit(chAtom(cps[0], nullptr, 0, curEmph), sax.pos);
          } else if (const uint32_t mapped = cidChar(cidFromSrc(src))) {
            emit(chAtom(mapped, nullptr, 0, curEmph), sax.pos);
          }
        } else if (pictureBody && textCount < 2000) {
          uint16_t id = 0;
          if (sink.picture && src[0]) {
            id = sink.picture(sink.ctx, src);
          }
          Atom pic{};
          if (id != 0) {
            pic.kind = AtomKind::Picture;
            pic.cp = id;
          } else {
            pic.kind = AtomKind::PageBreak;
          }
          emit(pic, sax.pos);
        } else if (nCp > 0 && (inHeading || heightEmOf(sax) > 0 || nCp <= 8)) {
          uint32_t digits[4]{};
          uint8_t dN = 0;
          auto flushDigits = [&]() {
            if (dN == 0) {
              return;
            }
            if (dN >= 2) {
              Atom t{};
              t.kind = AtomKind::Tcy;
              t.cp = digits[0];
              t.tcyCount = dN;
              t.emphasis = curEmph;
              for (uint8_t i = 0; i < dN; ++i) {
                t.tcy[i] = digits[i];
              }
              emit(t, sax.pos);
            } else {
              emit(chAtom(digits[0], nullptr, 0, curEmph), sax.pos);
            }
            dN = 0;
          };
          for (uint8_t i = 0; i < nCp; ++i) {
            if (cps[i] >= '0' && cps[i] <= '9' && dN < 4) {
              digits[dN++] = cps[i];
              continue;
            }
            flushDigits();
            if (!isPlaceholder(cps[i])) {
              emit(chAtom(cps[i], nullptr, 0, curEmph), sax.pos);
            }
          }
          flushDigits();
        }
      }
      continue;
    }
    if (sax.kind == HtmlSax::Kind::End) {
      if (isSkip(sax.name) && skipDepth > 0) {
        --skipDepth;
        continue;
      }
      if (skipDepth == 0) {
        if (emphN > 0 && emphFrames[emphN - 1].depth == depth) {
          curEmph = emphFrames[emphN - 1].prev;
          --emphN;
        }
        if (depth > 0) {
          --depth;
        }
      }
      if (eq(sax.name, "rt")) {
        inRt = false;
        if (rubyBaseN > 0) {
          flushRubyPair(sax.pos);
        }
      } else if (eq(sax.name, "rb")) {
        inRb = false;
      } else if (eq(sax.name, "ruby")) {
        if (rubyBaseN > 0) {
          flushRubyPair(sax.pos);
        }
        if (rubyDepth > 0) {
          --rubyDepth;
        }
        inRt = false;
        inRb = false;
      } else if (eq(sax.name, "span") && tcyDepth > 0) {
        --tcyDepth;
        if (tcyN > 0) {
          Atom t{};
          t.kind = AtomKind::Tcy;
          t.cp = tcyCps[0];
          t.tcyCount = tcyN;
          t.emphasis = curEmph;
          for (uint8_t i = 0; i < tcyN && i < 4; ++i) {
            t.tcy[i] = tcyCps[i];
          }
          emit(t, sax.pos);
        }
        tcyN = 0;
      } else if (isHeading(sax.name)) {
        capturingTitle = false;
        inHeading = false;
        Atom c{};
        c.kind = AtomKind::ColumnBreak;
        emit(c, sax.pos);
      } else if (isPara(sax.name) || eq(sax.name, "div")) {
        capturingTitle = false;
        if (tocLike && textCount < 2000 && isPara(sax.name)) {
          Atom c{};
          c.kind = AtomKind::ColumnBreak;
          c.startEm = static_cast<uint8_t>(indent);
          emit(c, sax.pos);
        } else {
          Atom g{};
          g.kind = AtomKind::Gap;
          emit(g, sax.pos);
        }
      }
      if (indentTop > 0) {
        indent = indentStack[--indentTop];
      }
    }
  }

  if (tocLike && textCount < 2000) {
    res.compactColumns = true;
  }
  if (titleBuf[0]) {
    snprintf(res.title, sizeof(res.title), "%s", titleBuf);
  }
  (void)inBody;
  return true;
}

}  // namespace ts
