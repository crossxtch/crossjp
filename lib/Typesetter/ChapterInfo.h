#pragma once

#include <cstdint>
#include <string>

namespace ts {

struct ChapterInfo {
  std::string name;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  // False until this chapter's atom offset is inside the indexed range.
  bool ready = false;
};

}  // namespace ts
