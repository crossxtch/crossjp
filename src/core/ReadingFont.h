#pragma once

#include <XgfFont.h>

#include <cstddef>

// Installed .xgf2 faces under /.crossjp/fonts/. One is selected in Settings
// and used for UI CJK and .txt/.epub. Only that file is loaded in RAM.
namespace ReadingFont {

constexpr const char* kDir = "/.crossjp/fonts";
constexpr const char* kLegacyPath = XgfFont::kDefaultPath;
constexpr size_t kMaxFileName = 79;

// Move a leftover /.crossjp/reading.xgf2 into kDir.
void migrate();

bool isFontFilename(const char* name);
bool copyFilename(char* out, size_t outSize, const char* name);
void makePath(char* out, size_t outSize, const char* filename);

// Absolute path of the selected face, or the first installed one. false if none.
bool activePath(char* out, size_t outSize);
bool setActive(const char* filename);
bool loadUi(XgfFont& font);

}  // namespace ReadingFont
