#include "core/UiText.h"

#include <cstring>

namespace uiText {

#define UI_PTR(name, text) const char* name = text;
UI_STRINGS(UI_PTR)
#undef UI_PTR

const char* error(const char* en) {
  if (!en || en[0] == '\0') {
    return "";
  }
  static const struct {
    const char* key;
    const char* text;
  } kMap[] = {
      {"file not found", fileNotFound},
      {"no path", fileNotFound},
      {"text missing", fileNotFound},
      {"out of memory", outOfMemory},
      {"font missing", fontMissing},
      {"not loaded", fontMissing},
      {"font maps", fontMissing},
      {"invalid firmware", invalidFirmware},
      {"write failed", writeFailed},
      {"could not open file", couldNotOpenFile},
      {"could not read file", couldNotReadFile},
      {"read error", couldNotReadFile},
      {"not an ESP32 image", invalidFormat},
      {"unsupported book", invalidFormat},
      {"file too small", fileTooSmall},
      {"file too large", fileTooLarge},
      {"wrong device", wrongDevice},
      {"SD card error", sdCardError},
  };
  for (const auto& e : kMap) {
    if (strcmp(en, e.key) == 0) {
      return e.text;
    }
  }
  return unknownError;
}

}  // namespace uiText
