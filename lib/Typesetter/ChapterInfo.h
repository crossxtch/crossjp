#pragma once

#include <cstdint>
#include <string>

namespace ts {

struct ChapterInfo {
  std::string name;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
};

}  // namespace ts
