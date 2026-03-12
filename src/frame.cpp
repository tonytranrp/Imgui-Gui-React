#include "igr/frame.hpp"

namespace igr {
namespace {

std::size_t count_widgets(const WidgetNode& node) noexcept {
  std::size_t count = 1;
  for (const auto& child : node.children) {
    count += count_widgets(child);
  }
  return count;
}

}  // namespace

std::size_t FrameDocument::widget_count() const noexcept {
  std::size_t count = 0;
  for (const auto& root : roots) {
    count += count_widgets(root);
  }
  return count;
}

std::string_view to_string(WidgetKind kind) noexcept {
  switch (kind) {
    case WidgetKind::window:
      return "window";
    case WidgetKind::stack:
      return "stack";
    case WidgetKind::clip_rect:
      return "clip_rect";
    case WidgetKind::text:
      return "text";
    case WidgetKind::button:
      return "button";
    case WidgetKind::checkbox:
      return "checkbox";
    case WidgetKind::image:
      return "image";
    case WidgetKind::progress_bar:
      return "progress_bar";
    case WidgetKind::separator:
      return "separator";
    case WidgetKind::custom_draw:
      return "custom_draw";
  }
  return "unknown";
}

std::string_view to_string(Axis axis) noexcept {
  switch (axis) {
    case Axis::horizontal:
      return "horizontal";
    case Axis::vertical:
      return "vertical";
  }
  return "vertical";
}

std::string_view to_string(CustomDrawPrimitive primitive) noexcept {
  switch (primitive) {
    case CustomDrawPrimitive::fill_rect:
      return "fill_rect";
    case CustomDrawPrimitive::stroke_rect:
      return "stroke_rect";
    case CustomDrawPrimitive::line:
      return "line";
    case CustomDrawPrimitive::shader_rect:
      return "shader_rect";
    case CustomDrawPrimitive::shader_image:
      return "shader_image";
  }
  return "fill_rect";
}


bool parse_custom_draw_primitive(std::string_view value, CustomDrawPrimitive* primitive) noexcept {
  if (primitive == nullptr) {
    return false;
  }
  if (value == "fill_rect") {
    *primitive = CustomDrawPrimitive::fill_rect;
    return true;
  }
  if (value == "stroke_rect") {
    *primitive = CustomDrawPrimitive::stroke_rect;
    return true;
  }
  if (value == "line") {
    *primitive = CustomDrawPrimitive::line;
    return true;
  }
  if (value == "shader_rect") {
    *primitive = CustomDrawPrimitive::shader_rect;
    return true;
  }
  if (value == "shader_image") {
    *primitive = CustomDrawPrimitive::shader_image;
    return true;
  }
  return false;
}

}  // namespace igr
