#include "igr/interaction.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>

namespace igr {
namespace {

std::optional<float> attr_float(const WidgetNode& node, std::string_view name) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name != name) {
      continue;
    }
    float value = 0.0f;
    const auto result = std::from_chars(attribute.value.data(), attribute.value.data() + attribute.value.size(), value);
    if (result.ec == std::errc()) {
      return value;
    }
  }
  return std::nullopt;
}

bool attr_bool(const WidgetNode& node, std::string_view name, bool fallback) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name != name) {
      continue;
    }
    if (attribute.value == "true") {
      return true;
    }
    if (attribute.value == "false") {
      return false;
    }
  }
  return fallback;
}

Rect window_rect(const WidgetNode& node) {
  return {
      .origin = {attr_float(node, "x").value_or(32.0f), attr_float(node, "y").value_or(32.0f)},
      .extent = {
          std::max(0.0f, attr_float(node, "width").value_or(420.0f)),
          std::max(0.0f, attr_float(node, "height").value_or(260.0f)),
      },
  };
}

Rect intersect_rect(Rect lhs, Rect rhs) {
  const float left = std::max(lhs.origin.x, rhs.origin.x);
  const float top = std::max(lhs.origin.y, rhs.origin.y);
  const float right = std::min(lhs.origin.x + lhs.extent.x, rhs.origin.x + rhs.extent.x);
  const float bottom = std::min(lhs.origin.y + lhs.extent.y, rhs.origin.y + rhs.extent.y);
  return {
      .origin = {left, top},
      .extent = {std::max(0.0f, right - left), std::max(0.0f, bottom - top)},
  };
}

std::optional<Rect> intersect_clip(const std::optional<Rect>& current_clip, Rect next_clip) {
  if (!current_clip.has_value()) {
    return next_clip;
  }
  return intersect_rect(*current_clip, next_clip);
}

bool point_in_rect(Vec2 point, Rect rect) {
  return point.x >= rect.origin.x && point.y >= rect.origin.y && point.x <= rect.origin.x + rect.extent.x &&
         point.y <= rect.origin.y + rect.extent.y;
}

float measure(const WidgetNode& node, float item_spacing) {
  switch (node.kind) {
    case WidgetKind::text:
      return 26.0f;
    case WidgetKind::button:
      return 34.0f;
    case WidgetKind::clip_rect: {
      const float explicit_height = attr_float(node, "height").value_or(0.0f);
      if (explicit_height > 0.0f) {
        return explicit_height;
      }
      float total = 0.0f;
      for (std::size_t index = 0; index < node.children.size(); ++index) {
        total += measure(node.children[index], item_spacing);
        if (index + 1 < node.children.size()) {
          total += item_spacing;
        }
      }
      return total;
    }
    case WidgetKind::checkbox:
      return 26.0f;
    case WidgetKind::image:
      return attr_float(node, "height").value_or(72.0f) + (node.label.empty() ? 0.0f : 24.0f);
    case WidgetKind::progress_bar:
      return 42.0f;
    case WidgetKind::separator:
      return 6.0f;
    case WidgetKind::custom_draw:
      return std::max(1.0f, attr_float(node, "layout_height").value_or(1.0f));
    case WidgetKind::stack: {
      float total = 0.0f;
      for (std::size_t index = 0; index < node.children.size(); ++index) {
        total += measure(node.children[index], item_spacing);
        if (index + 1 < node.children.size()) {
          total += item_spacing;
        }
      }
      return total;
    }
    case WidgetKind::window:
      return window_rect(node).extent.y;
  }
  return 0.0f;
}

bool is_horizontal_stack(const WidgetNode& node) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name == "axis") {
      return attribute.value == "horizontal";
    }
  }
  return false;
}

void push_region(InteractionMap& map, const WidgetNode& node, Rect bounds, const std::optional<Rect>& clip_rect) {
  if (bounds.extent.x <= 0.0f || bounds.extent.y <= 0.0f) {
    return;
  }

  const bool button_enabled = node.kind != WidgetKind::button || attr_bool(node, "enabled", true);
  const bool interactive = (node.kind == WidgetKind::button && button_enabled) || node.kind == WidgetKind::checkbox || node.kind == WidgetKind::image;
  const bool keyboard_focusable = (node.kind == WidgetKind::button && button_enabled) || node.kind == WidgetKind::checkbox;
  if (node.kind == WidgetKind::window || interactive) {
    map.regions.push_back({
        .id = node.id,
        .kind = node.kind,
        .bounds = bounds,
        .clip_rect = clip_rect,
        .interactive = interactive,
        .keyboard_focusable = keyboard_focusable,
    });
  }
}

void build_regions(const WidgetNode& node, Rect area, float padding, float title_height, float item_spacing, InteractionMap& map,
                   const std::optional<Rect>& clip_rect) {
  switch (node.kind) {
    case WidgetKind::window: {
      const Rect rect = window_rect(node);
      push_region(map, node, rect, clip_rect);
      Rect inner{
          .origin = {rect.origin.x + padding, rect.origin.y + title_height + padding},
          .extent = {
              std::max(0.0f, rect.extent.x - padding * 2.0f),
              std::max(0.0f, rect.extent.y - title_height - padding * 2.0f),
          },
      };
      for (const auto& child : node.children) {
        build_regions(child, inner, padding, title_height, item_spacing, map, clip_rect);
      }
      return;
    }
    case WidgetKind::stack: {
      if (is_horizontal_stack(node) && !node.children.empty()) {
        const float width = std::max(
            0.0f,
            (area.extent.x - static_cast<float>(node.children.size() - 1) * item_spacing) / static_cast<float>(node.children.size()));
        float x = area.origin.x;
        for (const auto& child : node.children) {
          build_regions(child, {{x, area.origin.y}, {width, area.extent.y}}, padding, title_height, item_spacing, map, clip_rect);
          x += width + item_spacing;
        }
        return;
      }

      float y = area.origin.y;
      for (const auto& child : node.children) {
        const float height = measure(child, item_spacing);
        build_regions(child, {{area.origin.x, y}, {area.extent.x, height}}, padding, title_height, item_spacing, map, clip_rect);
        y += height + item_spacing;
      }
      return;
    }
    case WidgetKind::clip_rect: {
      Rect clip_area = area;
      if (const auto width = attr_float(node, "width")) {
        clip_area.extent.x = std::max(0.0f, std::min(*width, area.extent.x));
      }
      if (const auto height = attr_float(node, "height")) {
        clip_area.extent.y = std::max(0.0f, std::min(*height, area.extent.y));
      }
      const auto nested_clip = intersect_clip(clip_rect, clip_area);
      float y = clip_area.origin.y;
      for (const auto& child : node.children) {
        const float height = measure(child, item_spacing);
        build_regions(child, {{clip_area.origin.x, y}, {clip_area.extent.x, height}}, padding, title_height, item_spacing, map, nested_clip);
        y += height + item_spacing;
      }
      return;
    }
    default:
      push_region(map, node, area, clip_rect);
      return;
  }
}

}  // namespace

const WidgetNode* find_widget_by_id(const FrameDocument& document, WidgetId id) noexcept {
  if (id == 0) {
    return nullptr;
  }

  const auto find_in_node = [&](const WidgetNode& node, const auto& self) -> const WidgetNode* {
    if (node.id == id) {
      return &node;
    }
    for (const auto& child : node.children) {
      if (const WidgetNode* match = self(child, self); match != nullptr) {
        return match;
      }
    }
    return nullptr;
  };

  for (const auto& root : document.roots) {
    if (const WidgetNode* match = find_in_node(root, find_in_node); match != nullptr) {
      return match;
    }
  }
  return nullptr;
}

std::string_view find_widget_key(const FrameDocument& document, WidgetId id) noexcept {
  if (const WidgetNode* node = find_widget_by_id(document, id); node != nullptr) {
    return node->key;
  }
  return {};
}

InteractionMap build_interaction_map(const FrameDocument& document) {
  InteractionMap map;
  map.regions.reserve(document.widget_count());
  constexpr float kPadding = 18.0f;
  constexpr float kTitleHeight = 28.0f;
  constexpr float kItemSpacing = 8.0f;
  for (const auto& root : document.roots) {
    build_regions(root, {}, kPadding, kTitleHeight, kItemSpacing, map, std::nullopt);
  }
  return map;
}

CaptureDecision evaluate_capture(const FrameDocument& document, const BackendHostOptions& host, const PointerInputState& state) {
  CaptureDecision decision;
  if (host.input_mode == InputMode::none) {
    return decision;
  }

  const InteractionMap map = build_interaction_map(document);
  for (auto it = map.regions.rbegin(); it != map.regions.rend(); ++it) {
    const bool inside_bounds = point_in_rect(state.position, it->bounds);
    const bool inside_clip = !it->clip_rect.has_value() || point_in_rect(state.position, *it->clip_rect);
    if (!inside_bounds || !inside_clip) {
      continue;
    }

    if (it->kind == WidgetKind::window && decision.hovered_window_id == 0) {
      decision.hovered_window_id = it->id;
      decision.within_window = true;
    }

    if (decision.hovered_widget_id == 0) {
      decision.hovered_widget_id = it->id;
    }

    if (it->interactive && !decision.within_interactive_region) {
      decision.within_interactive_region = true;
      if (state.primary_down) {
        decision.active_widget_id = it->id;
      }
    }

    if (decision.within_window && decision.within_interactive_region) {
      break;
    }
  }

  decision.wants_mouse = decision.within_window;
  decision.wants_keyboard = decision.within_window && state.keyboard_requested;
  return decision;
}

InteractionUpdate update_interaction(const FrameDocument& document,
                                     const BackendHostOptions& host,
                                     const PointerInputState& state,
                                     InteractionState* interaction_state) {
  InteractionUpdate update;
  update.capture = evaluate_capture(document, host, state);
  if (interaction_state == nullptr) {
    return update;
  }

  const bool pressed_this_frame = state.primary_down && !interaction_state->primary_down;
  const bool released_this_frame = !state.primary_down && interaction_state->primary_down;
  const WidgetId previously_pressed = interaction_state->pressed_widget_id;

  interaction_state->hovered_widget_id = update.capture.hovered_widget_id;

  if (pressed_this_frame) {
    interaction_state->pressed_widget_id = update.capture.active_widget_id;
    update.pressed_widget_id = interaction_state->pressed_widget_id;
  } else if (released_this_frame) {
    update.released_widget_id = previously_pressed;
    if (previously_pressed != 0 && update.capture.within_interactive_region && update.capture.hovered_widget_id == previously_pressed) {
      update.clicked_widget_id = previously_pressed;
    }
    interaction_state->pressed_widget_id = 0;
  } else {
    update.pressed_widget_id = interaction_state->pressed_widget_id;
  }

  interaction_state->primary_down = state.primary_down;
  return update;
}

}  // namespace igr