#include "igr/react/document.hpp"

#include <array>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace igr::react {
namespace {

const PropertyValue* find_property(const ElementNode& element, std::string_view name) {
  for (const auto& property : element.props) {
    if (property.name == name) {
      return &property.value;
    }
  }
  return nullptr;
}

bool has_property(const ElementNode& element, std::string_view name) {
  return find_property(element, name) != nullptr;
}

std::string get_string(const ElementNode& element, std::string_view name, std::string_view fallback = {}) {
  const auto* property = find_property(element, name);
  if (property == nullptr) {
    return std::string(fallback);
  }

  if (const auto* value = std::get_if<std::string>(property)) {
    return *value;
  }

  return std::string(fallback);
}

double get_number(const ElementNode& element, std::string_view name, double fallback) {
  const auto* property = find_property(element, name);
  if (property == nullptr) {
    return fallback;
  }

  if (const auto* integer_value = std::get_if<std::int64_t>(property)) {
    return static_cast<double>(*integer_value);
  }

  if (const auto* floating_value = std::get_if<double>(property)) {
    return *floating_value;
  }

  return fallback;
}

bool get_bool(const ElementNode& element, std::string_view name, bool fallback) {
  const auto* property = find_property(element, name);
  if (property == nullptr) {
    return fallback;
  }

  if (const auto* value = std::get_if<bool>(property)) {
    return *value;
  }

  return fallback;
}

int hex_nibble(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  const char lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
  if (lowered >= 'a' && lowered <= 'f') {
    return lowered - 'a' + 10;
  }
  return -1;
}

ColorRgba parse_color(const ElementNode& element, std::string_view name, const ColorRgba& fallback) {
  const std::string value = get_string(element, name);
  if (value.empty() || value.front() != '#') {
    return fallback;
  }

  if (value.size() != 7 && value.size() != 9) {
    return fallback;
  }

  ColorRgba color = fallback;
  const auto decode = [&](std::size_t offset) -> std::optional<float> {
    const int hi = hex_nibble(value[offset]);
    const int lo = hex_nibble(value[offset + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    return static_cast<float>((hi << 4) | lo) / 255.0f;
  };

  const auto r = decode(1);
  const auto g = decode(3);
  const auto b = decode(5);
  if (!r.has_value() || !g.has_value() || !b.has_value()) {
    return fallback;
  }
  color[0] = *r;
  color[1] = *g;
  color[2] = *b;
  color[3] = value.size() == 9 ? decode(7).value_or(fallback[3]) : 1.0f;
  return color;
}

ColorRgba parse_vec4_components(const ElementNode& element, std::string_view prefix, const ColorRgba& fallback) {
  ColorRgba value = fallback;
  value[0] = static_cast<float>(get_number(element, std::string(prefix) + "x", fallback[0]));
  value[1] = static_cast<float>(get_number(element, std::string(prefix) + "y", fallback[1]));
  value[2] = static_cast<float>(get_number(element, std::string(prefix) + "z", fallback[2]));
  value[3] = static_cast<float>(get_number(element, std::string(prefix) + "w", fallback[3]));
  return value;
}

std::optional<ColorRgba> parse_vec4_string(std::string_view text) {
  ColorRgba value{0.0f, 0.0f, 0.0f, 0.0f};
  std::size_t count = 0;
  std::string owned(text);

  char* cursor = owned.data();
  char* end = owned.data() + owned.size();
  while (cursor < end && count < value.size()) {
    while (cursor < end && (std::isspace(static_cast<unsigned char>(*cursor)) != 0 || *cursor == ',' || *cursor == ';' || *cursor == '[' ||
                            *cursor == ']' || *cursor == '(' || *cursor == ')')) {
      ++cursor;
    }
    if (cursor >= end) {
      break;
    }

    char* parsed_end = nullptr;
    const float parsed = std::strtof(cursor, &parsed_end);
    if (parsed_end == cursor) {
      return std::nullopt;
    }

    value[count++] = parsed;
    cursor = parsed_end;
  }

  while (cursor < end) {
    if (std::isspace(static_cast<unsigned char>(*cursor)) == 0 && *cursor != ',' && *cursor != ';' && *cursor != '[' && *cursor != ']' &&
        *cursor != '(' && *cursor != ')') {
      return std::nullopt;
    }
    ++cursor;
  }

  if (count == 0) {
    return std::nullopt;
  }

  return value;
}

ColorRgba parse_shader_param(const ElementNode& element, std::string_view name, const ColorRgba& fallback) {
  const std::string encoded = get_string(element, name);
  if (!encoded.empty()) {
    if (const auto parsed = parse_vec4_string(encoded); parsed.has_value()) {
      return *parsed;
    }
    return fallback;
  }

  return parse_vec4_components(element, name, fallback);
}

ShaderUniformData parse_shader_uniforms(const ElementNode& element) {
  ShaderUniformData uniforms{};
  uniforms.tint = parse_color(element, "tint", {1.0f, 1.0f, 1.0f, 1.0f});
  uniforms.params[0] = parse_shader_param(element, "param0", {0.0f, 0.0f, 0.0f, 0.0f});
  uniforms.params[1] = parse_shader_param(element, "param1", {0.0f, 0.0f, 0.0f, 0.0f});
  uniforms.params[2] = parse_shader_param(element, "param2", {0.0f, 0.0f, 0.0f, 0.0f});
  uniforms.params[3] = parse_shader_param(element, "param3", {0.0f, 0.0f, 0.0f, 0.0f});
  return uniforms;
}

Status materialize_children(const ElementNode& element, FrameBuilder& builder) {
  for (const auto& child : element.children) {
    Status status = materialize(child, builder);
    if (!status) {
      return status;
    }
  }
  return Status::success();
}

Status materialize_shader_rect(const ElementNode& element, std::string_view key, FrameBuilder& builder) {
  const std::string shader = get_string(element, "shader", get_string(element, "effect"));
  const std::string texture = get_string(element, "texture");
  const std::string resource = get_string(element, "resource");
  const Rect bounds{
      .origin = {static_cast<float>(get_number(element, "x", 0.0)), static_cast<float>(get_number(element, "y", 0.0))},
      .extent = {static_cast<float>(get_number(element, "width", 0.0)), static_cast<float>(get_number(element, "height", 0.0))},
  };
  return builder.shader_rect(key, shader, bounds, texture, resource, parse_shader_uniforms(element));
}

Status materialize_shader_image(const ElementNode& element, std::string_view key, FrameBuilder& builder) {
  const std::string shader = get_string(element, "shader", get_string(element, "effect"));
  const std::string texture = get_string(element, "texture", get_string(element, "source"));
  const std::string label = get_string(element, "label");
  const std::string resource = get_string(element, "resource");
  const ExtentF size{
      static_cast<float>(get_number(element, "width", 120.0)),
      static_cast<float>(get_number(element, "height", 72.0)),
  };
  return builder.shader_image(key, shader, texture, size, label, resource, parse_shader_uniforms(element));
}

}  // namespace

Status materialize(const ElementNode& element, FrameBuilder& builder) {
  const std::string key = element.key.empty() ? element.type : element.key;

  if (element.type == "fragment" || element.type == "group") {
    return materialize_children(element, builder);
  }

  if (element.type == "window") {
    Rect bounds{
        .origin = {static_cast<float>(get_number(element, "x", 40.0)), static_cast<float>(get_number(element, "y", 40.0))},
        .extent = {static_cast<float>(get_number(element, "width", 420.0)), static_cast<float>(get_number(element, "height", 260.0))},
    };

    Status status = builder.begin_window(key, get_string(element, "title", "Window"), bounds);
    if (!status) {
      return status;
    }

    status = materialize_children(element, builder);
    if (!status) {
      return status;
    }

    return builder.end_container();
  }

  if (element.type == "stack") {
    const auto axis = get_string(element, "axis", "vertical") == "horizontal" ? Axis::horizontal : Axis::vertical;
    Status status = builder.begin_stack(key, axis);
    if (!status) {
      return status;
    }

    status = materialize_children(element, builder);
    if (!status) {
      return status;
    }

    return builder.end_container();
  }

  if (element.type == "clip_rect" || element.type == "clip") {
    Status status = builder.begin_clip_rect(
        key,
        {
            static_cast<float>(get_number(element, "width", 0.0)),
            static_cast<float>(get_number(element, "height", 0.0)),
        });
    if (!status) {
      return status;
    }

    status = materialize_children(element, builder);
    if (!status) {
      return status;
    }

    return builder.end_container();
  }

  if (element.type == "text") {
    return builder.text(key, get_string(element, "value", get_string(element, "text")), get_string(element, "font"));
  }

  if (element.type == "button") {
    return builder.button(key, get_string(element, "label", "Button"), get_bool(element, "enabled", true));
  }

  if (element.type == "checkbox") {
    return builder.checkbox(key, get_string(element, "label", "Checkbox"), get_bool(element, "checked", false));
  }

  if (element.type == "image") {
    return builder.image(
        key,
        get_string(element, "texture", get_string(element, "source")),
        {
            static_cast<float>(get_number(element, "width", 120.0)),
            static_cast<float>(get_number(element, "height", 72.0)),
        },
        get_string(element, "label"),
        get_string(element, "resource"));
  }

  if (element.type == "shader_rect") {
    return materialize_shader_rect(element, key, builder);
  }

  if (element.type == "shader_image") {
    return materialize_shader_image(element, key, builder);
  }

  if (element.type == "progress_bar" || element.type == "progress") {
    return builder.progress_bar(key, get_string(element, "label", "Progress"), static_cast<float>(get_number(element, "value", 0.0)));
  }

  if (element.type == "separator") {
    return builder.separator(key);
  }

  if (element.type == "fill_rect" || (element.type == "rect" && get_string(element, "mode", "fill") != "stroke")) {
    return builder.fill_rect(
        key,
        {
            .origin = {static_cast<float>(get_number(element, "x", 0.0)), static_cast<float>(get_number(element, "y", 0.0))},
            .extent = {static_cast<float>(get_number(element, "width", 0.0)), static_cast<float>(get_number(element, "height", 0.0))},
        },
        parse_color(element, "color", {0.26f, 0.68f, 0.98f, 0.9f}));
  }

  if (element.type == "stroke_rect" || (element.type == "rect" && get_string(element, "mode", "fill") == "stroke")) {
    return builder.stroke_rect(
        key,
        {
            .origin = {static_cast<float>(get_number(element, "x", 0.0)), static_cast<float>(get_number(element, "y", 0.0))},
            .extent = {static_cast<float>(get_number(element, "width", 0.0)), static_cast<float>(get_number(element, "height", 0.0))},
        },
        parse_color(element, "color", {0.26f, 0.68f, 0.98f, 0.95f}),
        static_cast<float>(get_number(element, "thickness", 1.0)));
  }

  if (element.type == "line") {
    return builder.draw_line(
        key,
        {static_cast<float>(get_number(element, "x1", 0.0)), static_cast<float>(get_number(element, "y1", 0.0))},
        {static_cast<float>(get_number(element, "x2", 0.0)), static_cast<float>(get_number(element, "y2", 0.0))},
        parse_color(element, "color", {0.26f, 0.68f, 0.98f, 0.95f}),
        static_cast<float>(get_number(element, "thickness", 1.0)));
  }

  return Status::unsupported("Unsupported declarative element type: " + element.type);
}

}  // namespace igr::react
