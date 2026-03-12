#pragma once

#include <optional>
#include <vector>

#include "igr/frame.hpp"
#include "igr/host.hpp"

namespace igr {

struct InteractionRegion {
  WidgetId id{};
  WidgetKind kind{WidgetKind::text};
  Rect bounds{};
  std::optional<Rect> clip_rect;
  bool interactive{};
  bool keyboard_focusable{};
};

struct InteractionMap {
  std::vector<InteractionRegion> regions;
};

struct PointerInputState {
  Vec2 position{};
  bool primary_down{};
  bool keyboard_requested{};
};

struct CaptureDecision {
  WidgetId hovered_window_id{};
  WidgetId hovered_widget_id{};
  WidgetId active_widget_id{};
  bool within_window{};
  bool within_interactive_region{};
  bool wants_mouse{};
  bool wants_keyboard{};
};

struct InteractionState {
  WidgetId hovered_widget_id{};
  WidgetId pressed_widget_id{};
  bool primary_down{};
};

struct InteractionUpdate {
  CaptureDecision capture{};
  WidgetId pressed_widget_id{};
  WidgetId released_widget_id{};
  WidgetId clicked_widget_id{};
};

[[nodiscard]] InteractionMap build_interaction_map(const FrameDocument& document);
[[nodiscard]] CaptureDecision evaluate_capture(const FrameDocument& document, const BackendHostOptions& host, const PointerInputState& state);
[[nodiscard]] InteractionUpdate update_interaction(const FrameDocument& document,
                                                   const BackendHostOptions& host,
                                                   const PointerInputState& state,
                                                   InteractionState* interaction_state);
[[nodiscard]] const WidgetNode* find_widget_by_id(const FrameDocument& document, WidgetId id) noexcept;
[[nodiscard]] std::string_view find_widget_key(const FrameDocument& document, WidgetId id) noexcept;

}  // namespace igr