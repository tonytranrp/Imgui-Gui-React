#include "igr/context.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <sstream>
#include <system_error>
#include <utility>

namespace igr {
namespace {

constexpr WidgetId kFnvOffset = 1469598103934665603ull;
constexpr WidgetId kFnvPrime = 1099511628211ull;

WidgetId fnv_append(WidgetId hash, std::string_view text) noexcept {
  for (const char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

WidgetId make_widget_id(const std::vector<WidgetId>& scope_ids, WidgetKind kind, std::string_view key) noexcept {
  WidgetId hash = kFnvOffset;
  for (const auto scope_id : scope_ids) {
    std::array<char, 17> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), scope_id, 16);
    if (ec == std::errc{}) {
      hash = fnv_append(hash, std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
    }
  }

  hash = fnv_append(hash, to_string(kind));
  hash = fnv_append(hash, key.empty() ? "<anonymous>" : key);
  return hash;
}

std::string format_number(float value) {
  std::array<char, 32> buffer{};
  const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
  if (ec == std::errc{}) {
    return {buffer.data(), ptr};
  }

  std::ostringstream stream;
  stream << value;
  return stream.str();
}

void append_color(std::vector<WidgetAttribute>& attributes, std::string_view prefix, const ColorRgba& color) {
  attributes.push_back({std::string(prefix) + "r", format_number(color[0])});
  attributes.push_back({std::string(prefix) + "g", format_number(color[1])});
  attributes.push_back({std::string(prefix) + "b", format_number(color[2])});
  attributes.push_back({std::string(prefix) + "a", format_number(color[3])});
}

void append_shader_uniforms(std::vector<WidgetAttribute>& attributes, const ShaderUniformData& uniforms) {
  append_color(attributes, "tint_", uniforms.tint);
  for (std::size_t index = 0; index < uniforms.params.size(); ++index) {
    append_color(attributes, "param" + std::to_string(index) + "_", uniforms.params[index]);
  }
}

WidgetNode& append_node(FrameDocument& document, std::vector<WidgetNode*>& stack, WidgetNode node) {
  if (stack.empty()) {
    document.roots.push_back(std::move(node));
    return document.roots.back();
  }

  stack.back()->children.push_back(std::move(node));
  return stack.back()->children.back();
}

}  // namespace

Status FrameBuilder::begin_window(std::string_view key, std::string_view title, Rect bounds) {
  std::vector<WidgetAttribute> attributes;
  attributes.reserve(4);
  attributes.push_back({"x", format_number(bounds.origin.x)});
  attributes.push_back({"y", format_number(bounds.origin.y)});
  attributes.push_back({"width", format_number(bounds.extent.x)});
  attributes.push_back({"height", format_number(bounds.extent.y)});
  return push_container(WidgetKind::window, key, title, std::move(attributes));
}

Status FrameBuilder::begin_stack(std::string_view key, Axis axis) {
  return push_container(WidgetKind::stack, key, {}, {{"axis", std::string(to_string(axis))}});
}

Status FrameBuilder::begin_clip_rect(std::string_view key, ExtentF extent) {
  std::vector<WidgetAttribute> attributes;
  attributes.reserve(2);
  if (extent.x > 0.0f) {
    attributes.push_back({"width", format_number(extent.x)});
  }
  if (extent.y > 0.0f) {
    attributes.push_back({"height", format_number(extent.y)});
  }
  return push_container(WidgetKind::clip_rect, key, {}, std::move(attributes));
}

Status FrameBuilder::text(std::string_view key, std::string_view value, std::string_view font) {
  std::vector<WidgetAttribute> attributes;
  attributes.reserve(font.empty() ? 0 : 1);
  if (!font.empty()) {
    attributes.push_back({"font", std::string(font)});
  }
  return push_leaf(WidgetKind::text, key, value, std::move(attributes));
}

Status FrameBuilder::button(std::string_view key, std::string_view label, bool enabled) {
  return push_leaf(WidgetKind::button, key, label, {{"enabled", enabled ? "true" : "false"}});
}

Status FrameBuilder::checkbox(std::string_view key, std::string_view label, bool checked) {
  return push_leaf(WidgetKind::checkbox, key, label, {{"checked", checked ? "true" : "false"}});
}

Status FrameBuilder::image(std::string_view key, std::string_view texture, ExtentF size, std::string_view label, std::string_view resource) {
  std::vector<WidgetAttribute> attributes;
  attributes.reserve(resource.empty() ? 3 : 4);
  attributes.push_back({"texture", std::string(texture)});
  if (!resource.empty()) {
    attributes.push_back({"resource", std::string(resource)});
  }
  attributes.push_back({"width", format_number(size.x)});
  attributes.push_back({"height", format_number(size.y)});
  return push_leaf(WidgetKind::image, key, label, std::move(attributes));
}

Status FrameBuilder::progress_bar(std::string_view key, std::string_view label, float value) {
  value = std::clamp(value, 0.0f, 1.0f);
  return push_leaf(WidgetKind::progress_bar, key, label, {{"value", format_number(value)}});
}

Status FrameBuilder::separator(std::string_view key) {
  return push_leaf(WidgetKind::separator, key, {}, {});
}

Status FrameBuilder::fill_rect(std::string_view key, Rect bounds, const ColorRgba& color) {
  std::vector<WidgetAttribute> attributes;
  attributes.reserve(9);
  attributes.push_back({"x", format_number(bounds.origin.x)});
  attributes.push_back({"y", format_number(bounds.origin.y)});
  attributes.push_back({"width", format_number(bounds.extent.x)});
  attributes.push_back({"height", format_number(bounds.extent.y)});
  attributes.push_back({"layout_height", format_number(std::max(1.0f, bounds.origin.y + bounds.extent.y))});
  append_color(attributes, "color_", color);
  return push_custom_draw(key, CustomDrawPrimitive::fill_rect, std::move(attributes));
}

Status FrameBuilder::stroke_rect(std::string_view key, Rect bounds, const ColorRgba& color, float thickness) {
  if (thickness <= 0.0f) {
    return Status::invalid_argument("stroke_rect requires a positive line thickness.");
  }

  std::vector<WidgetAttribute> attributes;
  attributes.reserve(10);
  attributes.push_back({"x", format_number(bounds.origin.x)});
  attributes.push_back({"y", format_number(bounds.origin.y)});
  attributes.push_back({"width", format_number(bounds.extent.x)});
  attributes.push_back({"height", format_number(bounds.extent.y)});
  attributes.push_back({"thickness", format_number(thickness)});
  attributes.push_back({"layout_height", format_number(std::max(1.0f, bounds.origin.y + bounds.extent.y))});
  append_color(attributes, "color_", color);
  return push_custom_draw(key, CustomDrawPrimitive::stroke_rect, std::move(attributes));
}

Status FrameBuilder::draw_line(std::string_view key, Vec2 from, Vec2 to, const ColorRgba& color, float thickness) {
  if (thickness <= 0.0f) {
    return Status::invalid_argument("draw_line requires a positive line thickness.");
  }

  const float layout_height = std::max({1.0f, from.y, to.y}) + thickness;
  std::vector<WidgetAttribute> attributes;
  attributes.reserve(10);
  attributes.push_back({"x1", format_number(from.x)});
  attributes.push_back({"y1", format_number(from.y)});
  attributes.push_back({"x2", format_number(to.x)});
  attributes.push_back({"y2", format_number(to.y)});
  attributes.push_back({"thickness", format_number(thickness)});
  attributes.push_back({"layout_height", format_number(layout_height)});
  append_color(attributes, "color_", color);
  return push_custom_draw(key, CustomDrawPrimitive::line, std::move(attributes));
}

Status FrameBuilder::shader_rect(std::string_view key,
                                 std::string_view shader,
                                 Rect bounds,
                                 std::string_view texture,
                                 std::string_view resource,
                                 const ShaderUniformData& uniforms) {
  if (shader.empty()) {
    return Status::invalid_argument("shader_rect requires a non-empty shader key.");
  }

  std::vector<WidgetAttribute> attributes;
  attributes.reserve(25 + (texture.empty() ? 0 : 1) + (resource.empty() ? 0 : 1));
  attributes.push_back({"shader", std::string(shader)});
  if (!texture.empty()) {
    attributes.push_back({"texture", std::string(texture)});
  }
  if (!resource.empty()) {
    attributes.push_back({"resource", std::string(resource)});
  }
  attributes.push_back({"x", format_number(bounds.origin.x)});
  attributes.push_back({"y", format_number(bounds.origin.y)});
  attributes.push_back({"width", format_number(bounds.extent.x)});
  attributes.push_back({"height", format_number(bounds.extent.y)});
  attributes.push_back({"layout_height", format_number(std::max(1.0f, bounds.origin.y + bounds.extent.y))});
  append_shader_uniforms(attributes, uniforms);
  return push_custom_draw(key, CustomDrawPrimitive::shader_rect, std::move(attributes));
}

Status FrameBuilder::shader_image(std::string_view key,
                                  std::string_view shader,
                                  std::string_view texture,
                                  ExtentF size,
                                  std::string_view label,
                                  std::string_view resource,
                                  const ShaderUniformData& uniforms) {
  if (shader.empty()) {
    return Status::invalid_argument("shader_image requires a non-empty shader key.");
  }
  if (texture.empty() && resource.empty()) {
    return Status::invalid_argument("shader_image requires a texture or image resource key.");
  }

  std::vector<WidgetAttribute> attributes;
  attributes.reserve(24 + (texture.empty() ? 0 : 1) + (resource.empty() ? 0 : 1) + (label.empty() ? 0 : 1));
  attributes.push_back({"shader", std::string(shader)});
  if (!texture.empty()) {
    attributes.push_back({"texture", std::string(texture)});
  }
  if (!resource.empty()) {
    attributes.push_back({"resource", std::string(resource)});
  }
  if (!label.empty()) {
    attributes.push_back({"label", std::string(label)});
  }
  attributes.push_back({"x", "0"});
  attributes.push_back({"y", "0"});
  attributes.push_back({"width", format_number(size.x)});
  attributes.push_back({"height", format_number(size.y)});
  attributes.push_back({"layout_height", format_number(size.y + (label.empty() ? 0.0f : 24.0f))});
  append_shader_uniforms(attributes, uniforms);
  return push_custom_draw(key, CustomDrawPrimitive::shader_image, std::move(attributes));
}

Status FrameBuilder::end_container() {
  if (stack_.empty()) {
    return Status::invalid_argument("No open container is available to close.");
  }

  stack_.pop_back();
  scope_ids_.pop_back();
  return Status::success();
}

void FrameBuilder::reset(FrameDocument* document) {
  document_ = document;
  stack_.clear();
  scope_ids_.clear();
}

std::size_t FrameBuilder::open_container_count() const noexcept {
  return stack_.size();
}

void FrameBuilder::force_close() {
  stack_.clear();
  scope_ids_.clear();
}

Status FrameBuilder::push_container(
    WidgetKind kind,
    std::string_view key,
    std::string_view label,
    std::vector<WidgetAttribute> attributes) {
  if (document_ == nullptr) {
    return Status::not_ready("FrameBuilder is not attached to an active frame.");
  }

  WidgetNode node;
  node.kind = kind;
  node.key = std::string(key);
  node.label = std::string(label);
  node.id = make_widget_id(scope_ids_, kind, key);
  node.attributes = std::move(attributes);

  WidgetNode& inserted = append_node(*document_, stack_, std::move(node));
  stack_.push_back(&inserted);
  scope_ids_.push_back(inserted.id);
  return Status::success();
}

Status FrameBuilder::push_leaf(
    WidgetKind kind,
    std::string_view key,
    std::string_view label,
    std::vector<WidgetAttribute> attributes) {
  if (document_ == nullptr) {
    return Status::not_ready("FrameBuilder is not attached to an active frame.");
  }

  WidgetNode node;
  node.kind = kind;
  node.key = std::string(key);
  node.label = std::string(label);
  node.id = make_widget_id(scope_ids_, kind, key);
  node.attributes = std::move(attributes);

  append_node(*document_, stack_, std::move(node));
  return Status::success();
}

Status FrameBuilder::push_custom_draw(std::string_view key, CustomDrawPrimitive primitive, std::vector<WidgetAttribute> attributes) {
  attributes.insert(attributes.begin(), {"primitive", std::string(to_string(primitive))});
  return push_leaf(WidgetKind::custom_draw, key, {}, std::move(attributes));
}

Status UiContext::begin_frame(FrameInfo info) {
  if (frame_open_) {
    return Status::invalid_argument("A frame is already open. Call end_frame before beginning another frame.");
  }

  document_ = {};
  document_.info = info;
  builder_.reset(&document_);
  frame_open_ = true;
  return Status::success();
}

FrameBuilder& UiContext::builder() noexcept {
  return builder_;
}

FrameDocument UiContext::end_frame() {
  if (!frame_open_) {
    FrameDocument document;
    document.diagnostics.push_back("end_frame was called without a matching begin_frame.");
    return document;
  }

  if (builder_.open_container_count() > 0) {
    document_.diagnostics.push_back("Open containers were auto-closed at the end of the frame.");
    builder_.force_close();
  }

  builder_.reset(nullptr);
  frame_open_ = false;
  return std::move(document_);
}

const FrameDocument& UiContext::current_document() const noexcept {
  return document_;
}

}  // namespace igr
