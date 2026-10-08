#pragma once

#include "Layout.h"

#include <cstddef>
#include <cstdint>

namespace ts {

struct AtomSink {
  void* ctx = nullptr;
  bool (*emit)(void* ctx, const Atom& atom, uint32_t pos) = nullptr;
  // Full-page picture. Return a 1-based id, or 0 to keep a blank page break.
  uint16_t (*picture)(void* ctx, const char* src) = nullptr;
};

struct HtmlIRResult {
  WritingMode mode = WritingMode::VerticalRl;
  bool compactColumns = false;
  bool hasMode = false;
  char title[96]{};
};

// Convert 電書協/Calibre XHTML to the lazahata atom stream.
bool htmlToAtoms(const char* data, size_t len, AtomSink sink, HtmlIRResult* result = nullptr);
bool htmlToAtomsPull(int (*read)(void* ctx, char* dst, int max), void* ctx, AtomSink sink,
                     HtmlIRResult* result = nullptr);

// html/body class, else a `<style>` writing-mode. False when the item does not say.
bool sniffWritingMode(const char* data, size_t len, WritingMode& mode);

void splitReading(const uint32_t* rt, uint8_t rtN, uint8_t baseN, uint32_t* out, uint8_t* outCount /* per base, cap 4 */);

}  // namespace ts
