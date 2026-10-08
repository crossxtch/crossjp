#include "TextIr.h"

#include "Kinsoku.h"

namespace ts {
namespace {

constexpr uint32_t kBar = 0xFF5C;       // ｜
constexpr uint32_t kRubyOpen = 0x300A;  // 《
constexpr uint32_t kRubyClose = 0x300B; // 》
constexpr uint32_t kNoteOpen = 0xFF3B;  // ［
constexpr uint32_t kNoteClose = 0xFF3D; // ］
constexpr uint32_t kNoteMark = 0xFF03;  // ＃
constexpr uint32_t kQuoteOpen = 0x300C;
constexpr uint32_t kQuoteClose = 0x300D;

constexpr uint32_t kOwari[] = {0x7D42, 0x308F, 0x308A};             // 終わり
constexpr uint32_t kHonbunOwari[] = {0x672C, 0x6587, 0x7D42, 0x308F, 0x308A};
constexpr uint32_t kHonbunShuryo[] = {0x672C, 0x6587, 0x7D42, 0x4E86};
constexpr uint32_t kPage[] = {0x6539, 0x30DA, 0x30FC, 0x30B8};      // 改ページ
constexpr uint32_t kPage2[] = {0x6539, 0x9801};                     // 改頁
constexpr uint32_t kCho[] = {0x6539, 0x4E01};                       // 改丁
constexpr uint32_t kSpread[] = {0x6539, 0x898B, 0x958B, 0x304D};    // 改見開き
constexpr uint32_t kDan[] = {0x6539, 0x6BB5};                       // 改段
constexpr uint32_t kTcy[] = {0x7E26, 0x4E2D, 0x6A2A};               // 縦中横
constexpr uint32_t kBouten[] = {0x508D, 0x70B9};                    // 傍点
constexpr uint32_t kBousen[] = {0x508D, 0x7DDA};                    // 傍線
constexpr uint32_t kGoma[] = {0x30B4, 0x30DE};
constexpr uint32_t kGomaH[] = {0x3054, 0x307E};

bool isAozoraKanji(const uint32_t cp) {
  return cp == 0x3005 || cp == 0x3007 || cp == 0x303B || isKanji(cp);  // 々 〇 〻
}

bool isRubyBase(const uint32_t cp) { return isAozoraKanji(cp) || isHiragana(cp) || isKatakana(cp); }

bool eqSeq(const uint32_t* a, const uint8_t an, const uint32_t* b, const uint8_t bn) {
  if (an != bn) {
    return false;
  }
  for (uint8_t i = 0; i < an; ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

bool hasSeq(const uint32_t* s, const uint8_t n, const uint32_t* lit, const uint8_t ln) {
  if (ln == 0 || n < ln) {
    return false;
  }
  for (uint8_t i = 0; i + ln <= n; ++i) {
    if (eqSeq(s + i, ln, lit, ln)) {
      return true;
    }
  }
  return false;
}

bool endsSeq(const uint32_t* s, const uint8_t n, const uint32_t* lit, const uint8_t ln) {
  if (n < ln) {
    return false;
  }
  return eqSeq(s + (n - ln), ln, lit, ln);
}

uint8_t emphasisKind(const uint32_t* s, const uint8_t n) {
  if (hasSeq(s, n, kBousen, 2)) {
    return 2;
  }
  if (hasSeq(s, n, kBouten, 2) || hasSeq(s, n, kGoma, 2) || hasSeq(s, n, kGomaH, 2)) {
    return 1;
  }
  return 0;
}

bool pieceOf(const Atom& a, uint32_t* out, uint8_t& n, const bool plainOnly) {
  n = 0;
  if (a.kind == AtomKind::Ch) {
    if (plainOnly && a.rubyCount != 0) {
      return false;
    }
    out[0] = a.cp;
    n = 1;
    return true;
  }
  if (plainOnly || (a.kind != AtomKind::Tcy && a.kind != AtomKind::Group)) {
    return false;
  }
  n = a.tcyCount > 4 ? 4 : a.tcyCount;
  if (n == 0) {
    return false;
  }
  for (uint8_t i = 0; i < n; ++i) {
    out[i] = a.tcy[i] != 0 ? a.tcy[i] : a.cp;
  }
  return true;
}

bool quotedOne(const uint32_t* s, const uint8_t n, uint32_t& out) {
  int open = -1;
  int close = -1;
  for (uint8_t i = 0; i < n; ++i) {
    if (s[i] == kQuoteOpen && open < 0) {
      open = i;
    } else if (s[i] == kQuoteClose && open >= 0) {
      close = i;
      break;
    }
  }
  if (open < 0 || close != open + 2) {
    return false;
  }
  const uint32_t cp = s[open + 1];
  if (cp == '+' || cp == 0xFF0B) {
    return false;
  }
  out = cp;
  return true;
}

}  // namespace

void Utf8AtomReader::bind(HalFile* f) {
  file = f;
  pos = 0;
  resetParse();
}

void Utf8AtomReader::seek(const uint32_t byteOffset) {
  pos = byteOffset;
  resetParse();
  if (file) {
    file->seekSet(byteOffset);
  }
}

void Utf8AtomReader::resetParse() {
  eof = false;
  pendingN = 0;
  queueCount = 0;
  queueHead = 0;
  tailN = 0;
  emphasis = 0;
  tcyOpen = false;
  tcyN = 0;
  tcyAt = 0;
  stop = false;
  lastBreak = true;
}

void Utf8AtomReader::unread(const uint32_t cp, const uint32_t at) {
  if (pendingN >= 4) {
    return;
  }
  pendingCp[pendingN] = cp;
  pendingAt[pendingN] = at;
  ++pendingN;
}

bool Utf8AtomReader::popQueue(Atom& out, uint32_t& atomPos) {
  if (queueCount == 0) {
    return false;
  }
  out = queue[queueHead];
  atomPos = queuePos[queueHead];
  queueHead = static_cast<uint8_t>((queueHead + 1) % kQueueCap);
  --queueCount;
  return true;
}

void Utf8AtomReader::pushQueue(const Atom& a, const uint32_t atomPos) {
  if (queueCount >= kQueueCap) {
    return;
  }
  const uint8_t i = static_cast<uint8_t>((queueHead + queueCount) % kQueueCap);
  queue[i] = a;
  queuePos[i] = atomPos;
  ++queueCount;
}

void Utf8AtomReader::pushTail(const Atom& a, const uint32_t atomPos) {
  if (tailN >= kTailCap) {
    pushQueue(tail[0], tailPos[0]);
    lastBreak = false;
    for (uint8_t i = 1; i < tailN; ++i) {
      tail[i - 1] = tail[i];
      tailPos[i - 1] = tailPos[i];
    }
    --tailN;
  }
  if (tailN < kTailCap) {
    tail[tailN] = a;
    tailPos[tailN] = atomPos;
    ++tailN;
  }
}

void Utf8AtomReader::flushTail() {
  for (uint8_t i = 0; i < tailN; ++i) {
    pushQueue(tail[i], tailPos[i]);
    lastBreak = false;
  }
  tailN = 0;
}

void Utf8AtomReader::flushTcy(const bool close) {
  if (tcyN > 0) {
    Atom t{};
    t.kind = AtomKind::Tcy;
    t.cp = tcyBuf[0];
    t.tcyCount = tcyN;
    t.emphasis = emphasis;
    for (uint8_t i = 0; i < tcyN; ++i) {
      t.tcy[i] = tcyBuf[i];
    }
    const uint32_t at = tcyAt;
    tcyN = 0;
    pushTail(t, at);
  }
  if (close) {
    tcyOpen = false;
  }
}

void Utf8AtomReader::enqueueBreak(const Atom& a, const uint32_t atomPos) {
  flushTcy(true);
  flushTail();
  pushQueue(a, atomPos);
  lastBreak = true;
}

Atom Utf8AtomReader::makeCh(const uint32_t cp) const {
  Atom a{};
  a.kind = AtomKind::Ch;
  a.cp = cp;
  a.emphasis = emphasis;
  return a;
}

void Utf8AtomReader::pushChar(const uint32_t cp, const uint32_t at) {
  if (tcyOpen) {
    if (tcyN == 0) {
      tcyAt = at;
    }
    if (tcyN < 4) {
      tcyBuf[tcyN++] = cp;
    }
    if (tcyN == 4) {
      flushTcy(false);
    }
    return;
  }
  if (cp == ' ') {
    Atom s{};
    s.kind = AtomKind::Space;
    pushTail(s, at);
    return;
  }
  pushTail(makeCh(cp), at);
}

void Utf8AtomReader::appendRuby(const uint32_t* bases, const uint8_t baseN, const uint32_t* ruby, const uint8_t rubyN,
                                const uint32_t at) {
  if (baseN == 0) {
    return;
  }
  if (baseN == 1) {
    Atom a = makeCh(bases[0]);
    a.rubyCount = rubyN > 8 ? 8 : rubyN;
    for (uint8_t i = 0; i < a.rubyCount; ++i) {
      a.ruby[i] = ruby[i];
    }
    pushTail(a, at);
    return;
  }
  if (rubyN != baseN && baseN <= 4) {
    Atom g{};
    g.kind = AtomKind::Group;
    g.tcyCount = baseN;
    g.cp = bases[0];
    g.emphasis = emphasis;
    for (uint8_t i = 0; i < baseN; ++i) {
      g.tcy[i] = bases[i];
    }
    g.rubyCount = rubyN > 8 ? 8 : rubyN;
    for (uint8_t i = 0; i < g.rubyCount; ++i) {
      g.ruby[i] = ruby[i];
    }
    pushTail(g, at);
    return;
  }
  if (rubyN == baseN) {
    for (uint8_t i = 0; i < baseN; ++i) {
      Atom a = makeCh(bases[i]);
      a.rubyCount = 1;
      a.ruby[0] = ruby[i];
      pushTail(a, at);
    }
    return;
  }
  const uint8_t groupN = baseN > 4 ? 4 : baseN;
  Atom g{};
  g.kind = AtomKind::Group;
  g.tcyCount = groupN;
  g.cp = bases[0];
  g.emphasis = emphasis;
  for (uint8_t i = 0; i < groupN; ++i) {
    g.tcy[i] = bases[i];
  }
  g.rubyCount = rubyN > 8 ? 8 : rubyN;
  for (uint8_t i = 0; i < g.rubyCount; ++i) {
    g.ruby[i] = ruby[i];
  }
  pushTail(g, at);
  for (uint8_t i = groupN; i < baseN; ++i) {
    pushTail(makeCh(bases[i]), at);
  }
}

bool Utf8AtomReader::suffixMatch(const uint32_t* text, const uint8_t textN, uint8_t& from,
                                 const bool plainOnly) const {
  if (textN == 0 || tailN == 0) {
    return false;
  }
  uint32_t got[32]{};
  uint8_t gotN = 0;
  int idx = tailN;
  while (idx > 0 && gotN < textN) {
    --idx;
    uint32_t piece[4]{};
    uint8_t pn = 0;
    if (!pieceOf(tail[idx], piece, pn, plainOnly)) {
      return false;
    }
    if (gotN + pn > textN || gotN + pn > 32) {
      return false;
    }
    for (int i = static_cast<int>(gotN) - 1; i >= 0; --i) {
      got[static_cast<uint8_t>(i) + pn] = got[i];
    }
    for (uint8_t i = 0; i < pn; ++i) {
      got[i] = piece[i];
    }
    gotN = static_cast<uint8_t>(gotN + pn);
  }
  if (gotN != textN) {
    return false;
  }
  for (uint8_t i = 0; i < textN; ++i) {
    if (got[i] != text[i]) {
      return false;
    }
  }
  from = static_cast<uint8_t>(idx);
  return true;
}

void Utf8AtomReader::applyTcy(const uint32_t* text, const uint8_t textN) {
  uint8_t from = 0;
  if (!suffixMatch(text, textN, from, true)) {
    return;
  }
  const uint32_t at = tailPos[from];
  tailN = from;
  uint8_t i = 0;
  while (i < textN) {
    uint8_t n = static_cast<uint8_t>(textN - i);
    if (n > 4) {
      n = 4;
    }
    Atom t{};
    t.kind = AtomKind::Tcy;
    t.cp = text[i];
    t.tcyCount = n;
    t.emphasis = emphasis;
    for (uint8_t k = 0; k < n; ++k) {
      t.tcy[k] = text[i + k];
    }
    pushTail(t, at);
    i = static_cast<uint8_t>(i + n);
  }
}

void Utf8AtomReader::applyEmphasis(const uint32_t* text, const uint8_t textN, const uint8_t kind) {
  uint8_t from = 0;
  if (!suffixMatch(text, textN, from, false)) {
    return;
  }
  for (uint8_t i = from; i < tailN; ++i) {
    tail[i].emphasis = kind;
  }
}

void Utf8AtomReader::readNote(uint32_t* cps, const uint8_t cap, uint8_t& n) {
  n = 0;
  uint32_t cp = 0;
  uint32_t at = 0;
  while (readCp(cp, at)) {
    if (cp == kNoteClose) {
      break;
    }
    if (cp == '\n' || cp == '\r' || cp == '\t') {
      continue;
    }
    if (n < cap) {
      cps[n++] = cp;
    }
  }
  uint8_t begin = 0;
  while (begin < n && (cps[begin] == ' ' || cps[begin] == 0x3000)) {
    ++begin;
  }
  while (n > begin && (cps[n - 1] == ' ' || cps[n - 1] == 0x3000)) {
    --n;
  }
  if (begin > 0) {
    const uint8_t keep = static_cast<uint8_t>(n - begin);
    for (uint8_t i = 0; i < keep; ++i) {
      cps[i] = cps[begin + i];
    }
    n = keep;
  }
}

void Utf8AtomReader::handleNote(const uint32_t* note, const uint8_t n, const uint32_t at) {
  if (eqSeq(note, n, kHonbunOwari, 5) || eqSeq(note, n, kHonbunShuryo, 4)) {
    stop = true;
    return;
  }
  if (eqSeq(note, n, kPage, 4) || eqSeq(note, n, kPage2, 2) || eqSeq(note, n, kCho, 2) ||
      eqSeq(note, n, kSpread, 4)) {
    Atom a{};
    a.kind = AtomKind::PageBreak;
    enqueueBreak(a, at);
    return;
  }
  if (eqSeq(note, n, kDan, 2)) {
    Atom a{};
    a.kind = AtomKind::ColumnBreak;
    enqueueBreak(a, at);
    return;
  }
  if (n > 2 && note[0] == kQuoteOpen) {
    uint8_t end = 0;
    bool found = false;
    for (uint8_t i = 1; i < n; ++i) {
      if (note[i] == kQuoteClose) {
        end = i;
        found = true;
        break;
      }
    }
    if (found && end > 1 && end + 1 < n) {
      const uint32_t* text = note + 1;
      const uint8_t textN = static_cast<uint8_t>(end - 1);
      const uint32_t* rest = note + end + 1;
      const uint8_t restN = static_cast<uint8_t>(n - (end + 1));
      if (restN >= 4 && rest[0] == 0x306F && eqSeq(rest + 1, 3, kTcy, 3)) {
        applyTcy(text, textN);
        return;
      }
      if (restN >= 2 && rest[0] == 0x306B) {
        const uint8_t kind = emphasisKind(rest + 1, static_cast<uint8_t>(restN - 1));
        if (kind != 0) {
          applyEmphasis(text, textN, kind);
          return;
        }
      }
    }
  }
  if (endsSeq(note, n, kOwari, 3) && n > 3) {
    const uint8_t bodyN = static_cast<uint8_t>(n - 3);
    if (eqSeq(note, bodyN, kTcy, 3)) {
      flushTcy(true);
      return;
    }
    if (emphasisKind(note, bodyN) != 0) {
      emphasis = 0;
      return;
    }
    return;
  }
  if (eqSeq(note, n, kTcy, 3)) {
    flushTcy(true);
    tcyOpen = true;
    return;
  }
  const uint8_t kind = emphasisKind(note, n);
  if (kind != 0 && !hasSeq(note, n, &kQuoteOpen, 1)) {
    emphasis = kind;
  }
}

bool Utf8AtomReader::readExplicit(const uint32_t barAt, const uint32_t barCp) {
  uint32_t bases[8]{};
  uint8_t baseN = 0;
  uint32_t baseAt = barAt;
  uint32_t look = 0;
  uint32_t lookAt = 0;
  bool opened = false;
  while (baseN < 8 && readCp(look, lookAt)) {
    if (look == kRubyOpen) {
      opened = true;
      break;
    }
    if (look == '\n' || look == '\r' || look == kNoteOpen || !isRubyBase(look)) {
      unread(look, lookAt);
      break;
    }
    if (baseN == 0) {
      baseAt = lookAt;
    }
    bases[baseN++] = look;
  }
  if (!opened) {
    if (baseN == 0) {
      return false;
    }
    pushChar(barCp, barAt);
    for (uint8_t i = 0; i < baseN; ++i) {
      pushChar(bases[i], baseAt);
    }
    return true;
  }
  uint32_t ruby[8]{};
  uint8_t rubyN = 0;
  bool closed = false;
  while (readCp(look, lookAt)) {
    if (look == kRubyClose) {
      closed = true;
      break;
    }
    if (look == '\n' || look == '\r') {
      unread(look, lookAt);
      break;
    }
    if (rubyN < 8) {
      ruby[rubyN++] = look;
    }
  }
  if (!closed || baseN == 0) {
    pushChar(barCp, barAt);
    for (uint8_t i = 0; i < baseN; ++i) {
      pushChar(bases[i], baseAt);
    }
    pushChar(kRubyOpen, lookAt);
    for (uint8_t i = 0; i < rubyN; ++i) {
      pushChar(ruby[i], lookAt);
    }
    return true;
  }
  appendRuby(bases, baseN, ruby, rubyN, baseAt);
  return true;
}

bool Utf8AtomReader::takeImplicit(const uint32_t at) {
  uint8_t n = 0;
  while (n < tailN) {
    const Atom& a = tail[tailN - 1 - n];
    if (a.kind != AtomKind::Ch || a.rubyCount != 0 || !isAozoraKanji(a.cp)) {
      break;
    }
    ++n;
  }
  if (n == 0) {
    return false;
  }
  uint32_t ruby[8]{};
  uint8_t rubyN = 0;
  bool closed = false;
  uint32_t look = 0;
  uint32_t lookAt = 0;
  while (readCp(look, lookAt)) {
    if (look == kRubyClose) {
      closed = true;
      break;
    }
    if (look == '\n' || look == '\r') {
      unread(look, lookAt);
      break;
    }
    if (rubyN < 8) {
      ruby[rubyN++] = look;
    }
  }
  if (!closed) {
    pushChar(kRubyOpen, at);
    for (uint8_t i = 0; i < rubyN; ++i) {
      pushChar(ruby[i], at);
    }
    return true;
  }
  uint32_t bases[8]{};
  const uint32_t baseAt = tailPos[tailN - n];
  for (uint8_t i = 0; i < n; ++i) {
    bases[i] = tail[tailN - n + i].cp;
  }
  tailN = static_cast<uint8_t>(tailN - n);
  appendRuby(bases, n, ruby, rubyN, baseAt);
  return true;
}

void Utf8AtomReader::newline(const uint32_t at) {
  int extra = 0;
  uint32_t look = 0;
  uint32_t lookAt = 0;
  while (readCp(look, lookAt)) {
    if (look == '\r') {
      continue;
    }
    if (look == '\n') {
      ++extra;
      continue;
    }
    unread(look, lookAt);
    break;
  }
  if (tcyOpen || tcyN > 0) {
    flushTcy(true);
  }
  if (extra == 0) {
    return;
  }
  if (tailN == 0 && lastBreak) {
    return;
  }
  flushTail();
  if (!lastBreak) {
    Atom g{};
    g.kind = AtomKind::Gap;
    pushQueue(g, at);
    lastBreak = true;
  }
}

void Utf8AtomReader::ingest(const uint32_t cp, const uint32_t at) {
  if (cp == '\r') {
    return;
  }
  if (cp == '\n') {
    newline(at);
    return;
  }
  if (cp == '\f') {
    Atom a{};
    a.kind = AtomKind::PageBreak;
    enqueueBreak(a, at);
    return;
  }
  if (cp == 0x203B) {
    uint32_t bracket = 0;
    uint32_t bracketAt = 0;
    if (!readCp(bracket, bracketAt)) {
      pushChar(cp, at);
      return;
    }
    if (bracket != kNoteOpen) {
      unread(bracket, bracketAt);
      pushChar(cp, at);
      return;
    }
    uint32_t mark = 0;
    uint32_t markAt = 0;
    if (!readCp(mark, markAt) || (mark != kNoteMark && mark != '#')) {
      if (mark != 0) {
        unread(mark, markAt);
      }
      unread(bracket, bracketAt);
      pushChar(cp, at);
      return;
    }
    uint32_t note[48]{};
    uint8_t noteN = 0;
    readNote(note, 48, noteN);
    uint32_t gaiji = 0;
    if (quotedOne(note, noteN, gaiji)) {
      pushChar(gaiji, at);
    }
    return;
  }
  if (cp == kNoteOpen) {
    uint32_t mark = 0;
    uint32_t markAt = 0;
    if (!readCp(mark, markAt)) {
      pushChar(cp, at);
      return;
    }
    if (mark != kNoteMark && mark != '#') {
      unread(mark, markAt);
      pushChar(cp, at);
      return;
    }
    uint32_t note[48]{};
    uint8_t noteN = 0;
    readNote(note, 48, noteN);
    handleNote(note, noteN, at);
    return;
  }
  if (cp == kBar || cp == '|') {
    if (!readExplicit(at, cp)) {
      pushChar(cp, at);
    }
    return;
  }
  if (cp == kRubyOpen) {
    if (!takeImplicit(at)) {
      pushChar(cp, at);
    }
    return;
  }
  pushChar(cp, at);
}

bool Utf8AtomReader::readCp(uint32_t& cp, uint32_t& at) {
  if (pendingN > 0) {
    --pendingN;
    cp = pendingCp[pendingN];
    at = pendingAt[pendingN];
    return true;
  }
  if (!file || eof) {
    eof = true;
    return false;
  }
  at = pos;
  uint8_t buf[4];
  const int n = file->read(buf, 1);
  if (n != 1) {
    eof = true;
    return false;
  }
  pos += 1;
  const uint8_t b0 = buf[0];
  int extra = 0;
  uint32_t v = b0;
  if ((b0 & 0x80) == 0) {
    cp = b0;
    return true;
  }
  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    v = b0 & 0x1F;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    v = b0 & 0x0F;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    v = b0 & 0x07;
  } else {
    cp = 0xFFFD;
    return true;
  }
  if (file->read(buf, static_cast<size_t>(extra)) != extra) {
    eof = true;
    cp = 0xFFFD;
    return true;
  }
  pos += static_cast<uint32_t>(extra);
  for (int i = 0; i < extra; ++i) {
    if ((buf[i] & 0xC0) != 0x80) {
      cp = 0xFFFD;
      return true;
    }
    v = (v << 6) | (buf[i] & 0x3F);
  }
  cp = v;
  return true;
}

bool Utf8AtomReader::next(Atom& out, uint32_t& atomPos) {
  if (popQueue(out, atomPos)) {
    return true;
  }
  if (stop && tailN == 0 && tcyN == 0) {
    return false;
  }
  uint32_t cp = 0;
  uint32_t at = 0;
  while (!stop && readCp(cp, at)) {
    ingest(cp, at);
    if (popQueue(out, atomPos)) {
      return true;
    }
  }
  flushTcy(true);
  flushTail();
  return popQueue(out, atomPos);
}

}  // namespace ts
