#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "igr/geometry.hpp"

namespace igr {

using WidgetId = std::uint64_t;
using ColorRgba = std::array<float, 4>;

enum class Axis {
  horizontal,
  vertical,
};

enum class WidgetKind {
  window,
  stack,
  clip_rect,
  text,
  button,
  checkbox,
  image,
  progress_bar,
  separator,
  custom_draw,
};

enum class CustomDrawPrimitive {
  fill_rect,
  stroke_rect,
  line,
  shader_rect,
  shader_image,
};

struct WidgetAttribute {
  std::string name;
  std::string value;
};

struct WidgetNode {
  WidgetId id{};
  WidgetKind kind{WidgetKind::text};
  std::string key;
  std::string label;
  std::vector<WidgetAttribute> attributes;
  std::vector<WidgetNode> children;
};

struct FrameInfo {
  std::uint64_t frame_index{};
  ExtentU viewport{};
  double delta_seconds{};
  double time_seconds{};
};

struct FrameDocument {
  FrameInfo info{};
  std::vector<WidgetNode> roots;
  std::vector<std::string> diagnostics;

  [[nodiscard]] std::size_t widget_count() const noexcept;
};

[[nodiscard]] std::string_view to_string(WidgetKind kind) noexcept;
[[nodiscard]] std::string_view to_string(Axis axis) noexcept;
[[nodiscard]] std::string_view to_string(CustomDrawPrimitive primitive) noexcept;
bool parse_custom_draw_primitive(std::string_view value, CustomDrawPrimitive* primitive) noexcept;

}  // namespace igr
