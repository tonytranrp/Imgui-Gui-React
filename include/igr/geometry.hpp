#pragma once

#include <cstdint>

namespace igr {

struct ExtentU {
  std::uint32_t width{};
  std::uint32_t height{};
};

struct Vec2 {
  float x{};
  float y{};
};

using ExtentF = Vec2;

struct Rect {
  Vec2 origin{};
  Vec2 extent{};
};

}  // namespace igr
