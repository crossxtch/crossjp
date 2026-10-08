#pragma once

#include <HalStorage.h>

#include <cstdint>

#include "Layout.h"

namespace ts {

// Streaming UTF-8 → Atom. Aozora-lite: ｜base《ruby》, implicit 漢字《ruby》,
// ［＃…］ notes, and blank-line paragraphs. A single newline is a soft wrap.
class Utf8AtomReader {
 public:
  void bind(HalFile* file);
  void seek(uint32_t byteOffset);
  uint32_t position() const { return pos; }
  bool atEnd() const { return eof && pendingN == 0 && queueCount == 0 && tailN == 0; }

  // Fills `out` and records the starting byte offset of that atom in `atomPos`.
  bool next(Atom& out, uint32_t& atomPos);

 private:
  static constexpr uint8_t kQueueCap = 32;
  static constexpr uint8_t kTailCap = 8;

  HalFile* file = nullptr;
  uint32_t pos = 0;
  bool eof = false;
  uint32_t pendingCp[4]{};
  uint32_t pendingAt[4]{};
  uint8_t pendingN = 0;

  Atom queue[kQueueCap]{};
  uint32_t queuePos[kQueueCap]{};
  uint8_t queueCount = 0;
  uint8_t queueHead = 0;

  // Recent glyphs, held so a following ［＃「…」…］ or 《ruby》 can rewrite them.
  Atom tail[kTailCap]{};
  uint32_t tailPos[kTailCap]{};
  uint8_t tailN = 0;

  uint8_t emphasis = 0;
  bool tcyOpen = false;
  uint32_t tcyBuf[4]{};
  uint8_t tcyN = 0;
  uint32_t tcyAt = 0;
  bool stop = false;
  bool lastBreak = true;

  void resetParse();
  bool readCp(uint32_t& cp, uint32_t& at);
  void unread(uint32_t cp, uint32_t at);
  bool popQueue(Atom& out, uint32_t& atomPos);
  void pushQueue(const Atom& a, uint32_t atomPos);
  void pushTail(const Atom& a, uint32_t atomPos);
  void flushTail();
  void flushTcy(bool close);
  void enqueueBreak(const Atom& a, uint32_t atomPos);
  void ingest(uint32_t cp, uint32_t at);
  void newline(uint32_t at);
  void pushChar(uint32_t cp, uint32_t at);
  void handleNote(const uint32_t* note, uint8_t n, uint32_t at);
  void readNote(uint32_t* cps, uint8_t cap, uint8_t& n);
  bool readExplicit(uint32_t barAt, uint32_t barCp);
  bool takeImplicit(uint32_t at);
  void appendRuby(const uint32_t* bases, uint8_t baseN, const uint32_t* ruby, uint8_t rubyN, uint32_t at);
  bool suffixMatch(const uint32_t* text, uint8_t textN, uint8_t& from, bool plainOnly) const;
  void applyTcy(const uint32_t* text, uint8_t textN);
  void applyEmphasis(const uint32_t* text, uint8_t textN, uint8_t kind);
  Atom makeCh(uint32_t cp) const;
};

}  // namespace ts
