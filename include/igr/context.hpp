#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

#include "igr/frame.hpp"
#include "igr/resources.hpp"
#include "igr/result.hpp"

namespace igr {

class FrameBuilder {
 public:
  Status begin_window(std::string_view key, std::string_view title, Rect bounds);
  Status begin_stack(std::string_view key, Axis axis);
  Status begin_clip_rect(std::string_view key, ExtentF extent = {-1.0f, -1.0f});
  Status text(std::string_view key, std::string_view value, std::string_view font = {});
  Status button(std::string_view key, std::string_view label, bool enabled = true);
  Status checkbox(std::string_view key, std::string_view label, bool checked);
  Status image(std::string_view key, std::string_view texture, ExtentF size, std::string_view label = {}, std::string_view resource = {});
  Status progress_bar(std::string_view key, std::string_view label, float value);
  Status separator(std::string_view key);
  Status fill_rect(std::string_view key, Rect bounds, const ColorRgba& color);
  Status stroke_rect(std::string_view key, Rect bounds, const ColorRgba& color, float thickness = 1.0f);
  Status draw_line(std::string_view key, Vec2 from, Vec2 to, const ColorRgba& color, float thickness = 1.0f);
  Status shader_rect(std::string_view key,
                     std::string_view shader,
                     Rect bounds,
                     std::string_view texture = {},
                     std::string_view resource = {},
                     const ShaderUniformData& uniforms = {});
  Status shader_image(std::string_view key,
                      std::string_view shader,
                      std::string_view texture,
                      ExtentF size,
                      std::string_view label = {},
                      std::string_view resource = {},
                      const ShaderUniformData& uniforms = {});
  Status end_container();

 private:
  friend class UiContext;

  void reset(FrameDocument* document);
  [[nodiscard]] std::size_t open_container_count() const noexcept;
  void force_close();

  Status push_container(WidgetKind kind, std::string_view key, std::string_view label, std::vector<WidgetAttribute> attributes);
  Status push_leaf(WidgetKind kind, std::string_view key, std::string_view label, std::vector<WidgetAttribute> attributes);
  Status push_custom_draw(std::string_view key, CustomDrawPrimitive primitive, std::vector<WidgetAttribute> attributes);

  FrameDocument* document_{};
  std::vector<WidgetNode*> stack_;
  std::vector<WidgetId> scope_ids_;
  std::vector<std::size_t> child_ordinals_;
};

class UiContext {
 public:
  Status begin_frame(FrameInfo info);
  [[nodiscard]] FrameBuilder& builder() noexcept;
  [[nodiscard]] FrameDocument end_frame();
  [[nodiscard]] const FrameDocument& current_document() const noexcept;

 private:
  FrameDocument document_{};
  FrameBuilder builder_{};
  bool frame_open_{false};
};

}  // namespace igr
