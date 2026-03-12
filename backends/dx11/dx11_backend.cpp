#include "igr/backends/dx11_backend.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <d2d1.h>
#include <d2d1helper.h>
#include <d3dcompiler.h>
#include <dwrite.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "igr/detail/string_lookup.hpp"
#include "igr/shader_compiler.hpp"

namespace igr::backends {
namespace {

using Microsoft::WRL::ComPtr;
template <typename Value>
using StringMap = detail::TransparentStringMap<Value>;

struct QuadVertex {
  float position[2];
  float color[4];
  float uv[2];
};

struct Quad {
  std::array<Vec2, 4> points{};
  std::array<float, 4> color{};
  std::array<float, 4> uv{0.0f, 0.0f, 1.0f, 1.0f};
  std::string_view texture_key;
  std::string_view shader_key;
  std::array<ShaderVector4, 4> params{};
  std::optional<Rect> clip_rect;
};

enum class TextStyle { title, body, center };

struct TextLabel {
  const std::wstring* text{};
  D2D1_RECT_F rect{};
  std::array<float, 4> color{};
  TextStyle style{TextStyle::body};
  std::string_view font_key;
  std::optional<Rect> clip_rect;
};

struct Scene {
  std::vector<Quad> quads;
  std::vector<TextLabel> labels;
  StringMap<std::wstring>* wide_text_cache{};
};

struct DrawBatch {
  std::size_t start_vertex{};
  std::size_t vertex_count{};
  std::string_view texture_key;
  std::string_view shader_key;
  ShaderVector4 tint{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<ShaderVector4, 4> params{};
  Rect bounds{};
  std::optional<Rect> clip_rect;
};

struct ShaderConstants {
  ShaderVector4 tint{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<ShaderVector4, 4> params{
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
  };
  ShaderVector4 rect{0.0f, 0.0f, 0.0f, 0.0f};
  ShaderVector4 viewport_and_time{0.0f, 0.0f, 0.0f, 0.0f};
  ShaderVector4 frame_data{0.0f, 0.0f, 0.0f, 0.0f};
};

struct Dx11CompiledShader {
  ShaderResourceDesc descriptor;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11InputLayout> input_layout;
};

struct ScopeAccumulator {
  std::uint64_t call_count{};
  std::uint64_t total_microseconds{};
  std::uint64_t max_microseconds{};
};

D2D1_COLOR_F to_d2d_color(const std::array<float, 4>& color) {
  return D2D1::ColorF(color[0], color[1], color[2], color[3]);
}

std::string hex_hr(HRESULT hr) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
  return stream.str();
}

bool is_device_lost(HRESULT hr) noexcept {
  return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

std::string device_error_message(ID3D11Device* device, const char* prefix, HRESULT hr) {
  std::string message = std::string(prefix) + ": " + hex_hr(hr);
  if (is_device_lost(hr) && device != nullptr) {
    message += " (device removed reason: " + hex_hr(device->GetDeviceRemovedReason()) + ")";
  }
  return message;
}

void release_swap_chain_bindings(ID3D11DeviceContext* context) noexcept {
  if (context == nullptr) {
    return;
  }

  ID3D11RenderTargetView* null_rtv = nullptr;
  context->OMSetRenderTargets(1, &null_rtv, nullptr);

  ID3D11ShaderResourceView* null_srv = nullptr;
  context->PSSetShaderResources(0, 1, &null_srv);
  context->Flush();
}

bool same_extent(ExtentU lhs, ExtentU rhs) noexcept {
  return lhs.width == rhs.width && lhs.height == rhs.height;
}

std::wstring wide(std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(std::max(count, 0)), L'\0');
  if (count > 0) {
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
  }
  return out;
}

const std::wstring& cache_wide_text(StringMap<std::wstring>& cache, std::string_view text) {
  const auto existing = cache.find(text);
  if (existing != cache.end()) {
    return existing->second;
  }
  auto [inserted, _] = cache.emplace(std::string(text), wide(text));
  return inserted->second;
}

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

bool attr_bool(const WidgetNode& node, std::string_view name, bool fallback = false) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name == name) {
      return attribute.value == "true";
    }
  }
  return fallback;
}

void quad(std::vector<Quad>& quads, Rect rect, const std::array<float, 4>& color, std::optional<Rect> clip_rect);

void stroke_rect(std::vector<Quad>& quads,
                 Rect rect,
                 float thickness,
                 const std::array<float, 4>& color,
                 std::optional<Rect> clip_rect = std::nullopt) {
  if (rect.extent.x <= 0.0f || rect.extent.y <= 0.0f || thickness <= 0.0f) {
    return;
  }
  quad(quads, {{rect.origin.x, rect.origin.y}, {rect.extent.x, thickness}}, color, clip_rect);
  quad(quads, {{rect.origin.x, rect.origin.y + rect.extent.y - thickness}, {rect.extent.x, thickness}}, color, clip_rect);
  quad(quads, {{rect.origin.x, rect.origin.y}, {thickness, rect.extent.y}}, color, clip_rect);
  quad(quads, {{rect.origin.x + rect.extent.x - thickness, rect.origin.y}, {thickness, rect.extent.y}}, color, clip_rect);
}

std::string_view attr_string(const WidgetNode& node, std::string_view name, std::string_view fallback = {}) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name == name) {
      return attribute.value;
    }
  }
  return fallback;
}

std::array<float, 4> attr_color(const WidgetNode& node, std::string_view prefix, const std::array<float, 4>& fallback) {
  std::array<float, 4> color = fallback;
  bool updated = false;
  const auto assign = [&](std::size_t index, std::string_view suffix) {
    const auto value = attr_float(node, std::string(prefix) + std::string(suffix));
    if (value.has_value()) {
      color[index] = *value;
      updated = true;
    }
  };
  assign(0, "r");
  assign(1, "g");
  assign(2, "b");
  assign(3, "a");
  return updated ? color : fallback;
}

ColorRgba attr_vec4(const WidgetNode& node, std::string_view prefix, const ColorRgba& fallback = {0.0f, 0.0f, 0.0f, 0.0f}) {
  ColorRgba value = fallback;
  value[0] = attr_float(node, std::string(prefix) + "x").value_or(fallback[0]);
  value[1] = attr_float(node, std::string(prefix) + "y").value_or(fallback[1]);
  value[2] = attr_float(node, std::string(prefix) + "z").value_or(fallback[2]);
  value[3] = attr_float(node, std::string(prefix) + "w").value_or(fallback[3]);
  return value;
}

std::array<ShaderVector4, 4> attr_shader_params(const WidgetNode& node) {
  return {
      attr_color(node, "param0_", {0.0f, 0.0f, 0.0f, 0.0f}),
      attr_color(node, "param1_", {0.0f, 0.0f, 0.0f, 0.0f}),
      attr_color(node, "param2_", {0.0f, 0.0f, 0.0f, 0.0f}),
      attr_color(node, "param3_", {0.0f, 0.0f, 0.0f, 0.0f}),
  };
}

std::string_view attr_image_key(const WidgetNode& node) {
  const std::string_view resource_key = attr_string(node, "resource");
  if (!resource_key.empty()) {
    return resource_key;
  }
  return attr_string(node, "texture");
}

std::string_view attr_shader_key(const WidgetNode& node) {
  const std::string_view shader_key = attr_string(node, "shader");
  if (!shader_key.empty()) {
    return shader_key;
  }
  return attr_string(node, "effect");
}

bool is_valid_extent(ExtentF extent) noexcept {
  return std::isfinite(extent.x) && std::isfinite(extent.y) && extent.x >= 0.0f && extent.y >= 0.0f;
}

bool is_valid_rect(Rect rect) noexcept {
  return std::isfinite(rect.origin.x) && std::isfinite(rect.origin.y) && is_valid_extent(rect.extent);
}

Status validate_font_descriptor(std::string_view backend_name, const FontResourceDesc& descriptor) {
  if (!std::isfinite(descriptor.size) || descriptor.size <= 0.0f) {
    return Status::invalid_argument(std::string(backend_name) + " font registration requires a finite positive size.");
  }
  return Status::success();
}

Status validate_image_descriptor(std::string_view backend_name, const ImageResourceDesc& descriptor) {
  if (descriptor.texture_key.empty()) {
    return Status::invalid_argument(std::string(backend_name) + " image registration requires a backing texture key.");
  }
  if (!is_valid_extent(descriptor.size)) {
    return Status::invalid_argument(std::string(backend_name) + " image registration requires a finite non-negative size.");
  }
  if (!is_valid_rect(descriptor.uv)) {
    return Status::invalid_argument(std::string(backend_name) + " image registration requires finite non-negative UV coordinates.");
  }
  return Status::success();
}

void record_scope(StringMap<ScopeAccumulator>& scopes, std::string_view name, std::uint64_t duration_microseconds) {
  auto [it, _] = scopes.try_emplace(std::string(name), ScopeAccumulator{});
  ScopeAccumulator& scope = it->second;
  scope.call_count += 1;
  scope.total_microseconds += duration_microseconds;
  scope.max_microseconds = (std::max)(scope.max_microseconds, duration_microseconds);
}

std::uint64_t estimate_font_format_bytes(const StringMap<ComPtr<IDWriteTextFormat>>& formats) {
  return static_cast<std::uint64_t>(formats.size()) * sizeof(IDWriteTextFormat*);
}

std::uint64_t estimate_wide_text_cache_bytes(const StringMap<std::wstring>& cache) {
  std::uint64_t total = 0;
  for (const auto& [key, value] : cache) {
    total += static_cast<std::uint64_t>(key.capacity());
    total += static_cast<std::uint64_t>(value.capacity()) * sizeof(wchar_t);
  }
  return total;
}

std::uint64_t estimate_scene_bytes(const Scene& scene) {
  return static_cast<std::uint64_t>(scene.quads.capacity()) * sizeof(Quad) + static_cast<std::uint64_t>(scene.labels.capacity()) * sizeof(TextLabel);
}

std::uint64_t estimate_d3d11_buffer_bytes(ID3D11Buffer* buffer) {
  if (buffer == nullptr) {
    return 0;
  }
  D3D11_BUFFER_DESC desc{};
  buffer->GetDesc(&desc);
  return static_cast<std::uint64_t>(desc.ByteWidth);
}

IDXGIAdapter* resolve_dxgi_adapter(ID3D11Device* device, ComPtr<IDXGIAdapter>& adapter) {
  if (device == nullptr) {
    return nullptr;
  }
  ComPtr<IDXGIDevice> dxgi_device;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device)))) {
    return nullptr;
  }
  if (FAILED(dxgi_device->GetAdapter(&adapter))) {
    return nullptr;
  }
  return adapter.Get();
}

void trim_wide_text_cache(StringMap<std::wstring>& cache, const ResourceBudgetConfig& budgets) {
  if (budgets.max_cached_wide_strings == 0) {
    cache.clear();
    detail::release_storage(cache);
    return;
  }
  if (cache.size() <= budgets.max_cached_wide_strings) {
    return;
  }
  cache.clear();
  detail::release_storage(cache);
}

std::size_t retained_capacity_target(std::size_t active_count, std::size_t max_retained, std::size_t minimum_capacity) {
  if (max_retained == 0) {
    return 0;
  }
  const std::size_t slack_target = active_count + active_count / 2;
  const std::size_t bounded_target = (std::min)(max_retained, (std::max)(slack_target, minimum_capacity));
  return (std::max)(active_count, bounded_target);
}

template <typename T>
void trim_vector_capacity(std::vector<T>& storage, std::size_t max_retained, std::size_t minimum_capacity = 64) {
  if (max_retained == 0) {
    storage.clear();
    detail::release_storage(storage);
    return;
  }
  const std::size_t target_capacity = retained_capacity_target(storage.size(), max_retained, minimum_capacity);
  if (storage.capacity() <= target_capacity * 2) {
    return;
  }
  std::vector<T> trimmed;
  trimmed.reserve(target_capacity);
  for (auto& item : storage) {
    trimmed.push_back(std::move(item));
  }
  storage.swap(trimmed);
}

void trim_scratch_storage(Scene& scene,
                          std::vector<QuadVertex>& vertices,
                          std::vector<DrawBatch>& batches,
                          const ResourceBudgetConfig& budgets) {
  trim_vector_capacity(scene.quads, budgets.max_retained_scene_quads, 128);
  trim_vector_capacity(scene.labels, budgets.max_retained_text_labels, 64);
  trim_vector_capacity(vertices, budgets.max_retained_vertices, 256);
  trim_vector_capacity(batches, budgets.max_retained_batches, 64);
}

void trim_vertex_buffer(ID3D11DeviceContext* context,
                        ComPtr<ID3D11Buffer>& vertex_buffer,
                        std::size_t& vertex_capacity,
                        std::size_t active_vertex_count,
                        const ResourceBudgetConfig& budgets) {
  if (!vertex_buffer) {
    return;
  }
  const std::size_t retained_limit = budgets.max_retained_vertices;
  if (retained_limit == 0) {
    if (context != nullptr) {
      ID3D11Buffer* null_buffer = nullptr;
      const UINT stride = 0;
      const UINT offset = 0;
      context->IASetVertexBuffers(0, 1, &null_buffer, &stride, &offset);
    }
    vertex_buffer.Reset();
    vertex_capacity = 0;
    return;
  }
  const std::size_t target_capacity = retained_capacity_target(active_vertex_count, retained_limit, 256);
  if (vertex_capacity <= target_capacity * 2) {
    return;
  }

  if (context != nullptr) {
    ID3D11Buffer* null_buffer = nullptr;
    const UINT stride = 0;
    const UINT offset = 0;
    context->IASetVertexBuffers(0, 1, &null_buffer, &stride, &offset);
  }
  vertex_buffer.Reset();
  vertex_capacity = 0;
}

std::array<float, 4> multiply_color(const std::array<float, 4>& lhs, const std::array<float, 4>& rhs) {
  return {
      lhs[0] * rhs[0],
      lhs[1] * rhs[1],
      lhs[2] * rhs[2],
      lhs[3] * rhs[3],
  };
}

float clamp_non_negative(float value) {
  return std::max(0.0f, value);
}

Rect window_rect(const WidgetNode& node) {
  return {
      .origin = {attr_float(node, "x").value_or(32.0f), attr_float(node, "y").value_or(32.0f)},
      .extent = {clamp_non_negative(attr_float(node, "width").value_or(420.0f)),
                 clamp_non_negative(attr_float(node, "height").value_or(260.0f))},
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

bool same_optional_rect(const std::optional<Rect>& lhs, const std::optional<Rect>& rhs) {
  if (!lhs.has_value() || !rhs.has_value()) {
    return !lhs.has_value() && !rhs.has_value();
  }

  return lhs->origin.x == rhs->origin.x &&
         lhs->origin.y == rhs->origin.y &&
         lhs->extent.x == rhs->extent.x &&
         lhs->extent.y == rhs->extent.y;
}

bool same_vec4(const ColorRgba& lhs, const ColorRgba& rhs) {
  return lhs[0] == rhs[0] && lhs[1] == rhs[1] && lhs[2] == rhs[2] && lhs[3] == rhs[3];
}

float measure(const WidgetNode& node,
              const Dx11Theme& theme,
              const StringMap<ImageResourceDesc>& image_resources,
              const StringMap<FontResourceDesc>& font_descriptors) {
  switch (node.kind) {
    case WidgetKind::text: {
      const auto font_it = font_descriptors.find(attr_string(node, "font"));
      const float font_size = font_it != font_descriptors.end() ? font_it->second.size : theme.body_text_size;
      return std::max(26.0f, font_size + 12.0f);
    }
    case WidgetKind::button:
      return 34.0f;
    case WidgetKind::clip_rect: {
      const float explicit_height = attr_float(node, "height").value_or(0.0f);
      if (explicit_height > 0.0f) {
        return explicit_height;
      }
      float total = 0.0f;
      for (std::size_t i = 0; i < node.children.size(); ++i) {
          total += measure(node.children[i], theme, image_resources, font_descriptors);
        if (i + 1 < node.children.size()) {
          total += theme.item_spacing;
        }
      }
      return total;
    }
    case WidgetKind::checkbox:
      return 26.0f;
    case WidgetKind::image: {
      const auto image_it = image_resources.find(attr_image_key(node));
      const float fallback_height = image_it != image_resources.end() ? image_it->second.size.y : 72.0f;
      return attr_float(node, "height").value_or(fallback_height) + (node.label.empty() ? 0.0f : 24.0f);
    }
    case WidgetKind::progress_bar:
      return 42.0f;
    case WidgetKind::separator:
      return 6.0f;
    case WidgetKind::custom_draw:
      return std::max(1.0f, attr_float(node, "layout_height").value_or(1.0f));
    case WidgetKind::stack: {
      float total = 0.0f;
      for (std::size_t i = 0; i < node.children.size(); ++i) {
        total += measure(node.children[i], theme, image_resources, font_descriptors);
        if (i + 1 < node.children.size()) {
          total += theme.item_spacing;
        }
      }
      return total;
    }
    case WidgetKind::window:
      return window_rect(node).extent.y;
  }
  return 0.0f;
}

void quad(std::vector<Quad>& quads, Rect rect, const std::array<float, 4>& color, std::optional<Rect> clip_rect = std::nullopt) {
  if (rect.extent.x > 0.0f && rect.extent.y > 0.0f) {
    quads.push_back({
        .points = {{
            {rect.origin.x, rect.origin.y},
            {rect.origin.x, rect.origin.y + rect.extent.y},
            {rect.origin.x + rect.extent.x, rect.origin.y},
            {rect.origin.x + rect.extent.x, rect.origin.y + rect.extent.y},
        }},
        .color = color,
        .uv = {0.0f, 0.0f, 1.0f, 1.0f},
        .texture_key = {},
        .shader_key = {},
        .params = {},
        .clip_rect = std::move(clip_rect),
    });
  }
}

void image_quad(std::vector<Quad>& quads, Rect rect, std::string_view texture_key, std::optional<Rect> clip_rect = std::nullopt) {
  if (rect.extent.x > 0.0f && rect.extent.y > 0.0f) {
    Quad quad_item;
    quad_item.points = {{
        {rect.origin.x, rect.origin.y},
        {rect.origin.x, rect.origin.y + rect.extent.y},
        {rect.origin.x + rect.extent.x, rect.origin.y},
        {rect.origin.x + rect.extent.x, rect.origin.y + rect.extent.y},
    }};
    quad_item.color = {1.0f, 1.0f, 1.0f, 1.0f};
    quad_item.texture_key = texture_key;
    quad_item.shader_key = {};
    quad_item.params = {};
    quad_item.clip_rect = std::move(clip_rect);
    quads.push_back(std::move(quad_item));
  }
}

void line_quad(std::vector<Quad>& quads, Vec2 from, Vec2 to, float thickness, const std::array<float, 4>& color,
               std::optional<Rect> clip_rect = std::nullopt) {
  if (thickness <= 0.0f) {
    return;
  }
  const float dx = to.x - from.x;
  const float dy = to.y - from.y;
  const float length = std::sqrt(dx * dx + dy * dy);
  if (length <= 0.0001f) {
    return;
  }
  const float nx = -dy / length * (thickness * 0.5f);
  const float ny = dx / length * (thickness * 0.5f);
  quads.push_back({
      .points = {{
          {from.x + nx, from.y + ny},
          {from.x - nx, from.y - ny},
          {to.x + nx, to.y + ny},
          {to.x - nx, to.y - ny},
      }},
      .color = color,
      .uv = {0.0f, 0.0f, 1.0f, 1.0f},
      .texture_key = {},
      .shader_key = {},
      .params = {},
      .clip_rect = std::move(clip_rect),
  });
}

void label(Scene& scene,
           std::string_view text,
           D2D1_RECT_F rect,
           const std::array<float, 4>& color,
           TextStyle style,
           std::optional<Rect> clip_rect = std::nullopt,
           std::string_view font_key = {}) {
  if (!text.empty() && scene.wide_text_cache != nullptr) {
    scene.labels.push_back({&cache_wide_text(*scene.wide_text_cache, text), rect, color, style, font_key, std::move(clip_rect)});
  }
}

Rect quad_bounds(const Quad& quad_item) {
  float left = quad_item.points[0].x;
  float top = quad_item.points[0].y;
  float right = quad_item.points[0].x;
  float bottom = quad_item.points[0].y;
  for (const auto& point : quad_item.points) {
    left = std::min(left, point.x);
    top = std::min(top, point.y);
    right = std::max(right, point.x);
    bottom = std::max(bottom, point.y);
  }
  return {{left, top}, {std::max(0.0f, right - left), std::max(0.0f, bottom - top)}};
}

void accumulate_widget_stats(const WidgetNode& node, BackendFrameStats& stats) {
  ++stats.widget_count;
  if (node.kind == WidgetKind::custom_draw) {
    ++stats.custom_draw_count;
  }
  if (node.kind == WidgetKind::clip_rect) {
    ++stats.clip_rect_count;
  }
  for (const auto& child : node.children) {
    accumulate_widget_stats(child, stats);
  }
}

void emit(const WidgetNode& node,
          Rect area,
          const Dx11Theme& theme,
          const StringMap<ImageResourceDesc>& image_resources,
          const StringMap<FontResourceDesc>& font_descriptors,
          Scene& scene,
          const std::optional<Rect>& clip_rect);

void emit_stack(const WidgetNode& node,
                Rect area,
                const Dx11Theme& theme,
                const StringMap<ImageResourceDesc>& image_resources,
                const StringMap<FontResourceDesc>& font_descriptors,
                Scene& scene,
                const std::optional<Rect>& clip_rect) {
  bool horizontal = false;
  for (const auto& attribute : node.attributes) {
    if (attribute.name == "axis" && attribute.value == "horizontal") {
      horizontal = true;
      break;
    }
  }
  if (horizontal && !node.children.empty()) {
    const float available_width =
        clamp_non_negative(area.extent.x - static_cast<float>(node.children.size() - 1) * theme.item_spacing);
    const float width = available_width / static_cast<float>(node.children.size());
    float x = area.origin.x;
    for (const auto& child : node.children) {
      emit(child, {{x, area.origin.y}, {width, area.extent.y}}, theme, image_resources, font_descriptors, scene, clip_rect);
      x += width + theme.item_spacing;
    }
    return;
  }
  float y = area.origin.y;
  for (const auto& child : node.children) {
    const float height = measure(child, theme, image_resources, font_descriptors);
    emit(child, {{area.origin.x, y}, {area.extent.x, height}}, theme, image_resources, font_descriptors, scene, clip_rect);
    y += height + theme.item_spacing;
  }
}

void emit(const WidgetNode& node,
          Rect area,
          const Dx11Theme& theme,
          const StringMap<ImageResourceDesc>& image_resources,
          const StringMap<FontResourceDesc>& font_descriptors,
          Scene& scene,
          const std::optional<Rect>& clip_rect) {
  switch (node.kind) {
    case WidgetKind::window: {
      const Rect rect = window_rect(node);
      quad(scene.quads, rect, theme.window_background);
      quad(scene.quads, {{rect.origin.x, rect.origin.y}, {rect.extent.x, theme.window_title_height}}, theme.title_bar);
      stroke_rect(scene.quads, rect, 1.0f, theme.separator);
      quad(scene.quads, {{rect.origin.x + 10.0f, rect.origin.y + theme.window_title_height + 10.0f},
                         {rect.extent.x - 20.0f, rect.extent.y - theme.window_title_height - 20.0f}},
           theme.panel_background);
      stroke_rect(scene.quads,
                  {{rect.origin.x + 10.0f, rect.origin.y + theme.window_title_height + 10.0f},
                   {rect.extent.x - 20.0f, rect.extent.y - theme.window_title_height - 20.0f}},
                  1.0f,
                  theme.separator);
      label(scene, node.label,
            D2D1::RectF(rect.origin.x + 14.0f, rect.origin.y + 2.0f, rect.origin.x + rect.extent.x - 14.0f, rect.origin.y + theme.window_title_height),
            theme.text_primary, TextStyle::title);
      Rect inner{{rect.origin.x + theme.padding, rect.origin.y + theme.window_title_height + theme.padding},
                 {clamp_non_negative(rect.extent.x - theme.padding * 2.0f),
                  clamp_non_negative(rect.extent.y - theme.window_title_height - theme.padding * 2.0f)}};
      for (const auto& child : node.children) {
        emit(child, inner, theme, image_resources, font_descriptors, scene, clip_rect);
      }
      return;
    }
    case WidgetKind::stack:
      emit_stack(node, area, theme, image_resources, font_descriptors, scene, clip_rect);
      return;
    case WidgetKind::clip_rect: {
      Rect clip_area = area;
      if (const auto width = attr_float(node, "width")) {
        clip_area.extent.x = std::clamp(*width, 0.0f, clamp_non_negative(area.extent.x));
      }
      if (const auto height = attr_float(node, "height")) {
        clip_area.extent.y = std::clamp(*height, 0.0f, clamp_non_negative(area.extent.y));
      }
      emit_stack(node, clip_area, theme, image_resources, font_descriptors, scene, intersect_clip(clip_rect, clip_area));
      return;
    }
    case WidgetKind::text:
      quad(scene.quads, {{area.origin.x, area.origin.y + 5.0f}, {3.0f, 16.0f}}, theme.accent, clip_rect);
      label(scene, node.label,
            D2D1::RectF(area.origin.x + 12.0f, area.origin.y + 1.0f, area.origin.x + area.extent.x - 8.0f, area.origin.y + 24.0f), theme.text_primary,
            TextStyle::body, clip_rect, attr_string(node, "font"));
      return;
    case WidgetKind::button:
      quad(scene.quads, {{area.origin.x, area.origin.y}, {area.extent.x, 34.0f}}, theme.button_background, clip_rect);
      quad(scene.quads, {{area.origin.x + 2.0f, area.origin.y + 2.0f}, {area.extent.x - 4.0f, 30.0f}}, theme.button_highlight, clip_rect);
      stroke_rect(scene.quads, {{area.origin.x, area.origin.y}, {area.extent.x, 34.0f}}, 1.0f, theme.separator, clip_rect);
      label(scene, node.label,
            D2D1::RectF(area.origin.x + 8.0f, area.origin.y + 2.0f, area.origin.x + area.extent.x - 8.0f, area.origin.y + 32.0f), theme.text_primary,
            TextStyle::center, clip_rect);
      return;
    case WidgetKind::checkbox:
      quad(scene.quads, {{area.origin.x, area.origin.y + 3.0f}, {18.0f, 18.0f}}, theme.checkbox_border, clip_rect);
      quad(scene.quads, {{area.origin.x + 1.0f, area.origin.y + 4.0f}, {16.0f, 16.0f}}, theme.panel_background, clip_rect);
      if (attr_bool(node, "checked")) {
        line_quad(scene.quads, {area.origin.x + 4.0f, area.origin.y + 12.0f}, {area.origin.x + 8.0f, area.origin.y + 16.0f}, 2.0f, theme.checkbox_fill,
                  clip_rect);
        line_quad(scene.quads, {area.origin.x + 8.0f, area.origin.y + 16.0f}, {area.origin.x + 15.0f, area.origin.y + 8.0f}, 2.0f, theme.checkbox_fill,
                  clip_rect);
      }
      label(scene, node.label,
            D2D1::RectF(area.origin.x + 28.0f, area.origin.y, area.origin.x + area.extent.x, area.origin.y + 24.0f), theme.text_primary, TextStyle::body,
            clip_rect);
      return;
    case WidgetKind::image: {
      const std::string_view image_key = attr_image_key(node);
      const ImageResourceDesc* resource = nullptr;
      if (!image_key.empty()) {
        const auto resource_it = image_resources.find(image_key);
        if (resource_it != image_resources.end()) {
          resource = &resource_it->second;
        }
      }
      const float width =
          std::clamp(attr_float(node, "width").value_or(resource ? resource->size.x : area.extent.x), 0.0f, clamp_non_negative(area.extent.x));
      const float height = clamp_non_negative(attr_float(node, "height").value_or(resource ? resource->size.y : 72.0f));
      const Rect image_rect{{area.origin.x, area.origin.y}, {width, height}};
      quad(scene.quads, image_rect, theme.panel_background, clip_rect);
      quad(scene.quads, {{image_rect.origin.x, image_rect.origin.y}, {image_rect.extent.x, 2.0f}}, theme.accent, clip_rect);
      stroke_rect(scene.quads, image_rect, 1.0f, theme.separator, clip_rect);
      Quad preview_quad{};
      preview_quad.points = {{
          {image_rect.origin.x + 2.0f, image_rect.origin.y + 2.0f},
          {image_rect.origin.x + 2.0f, image_rect.origin.y + image_rect.extent.y - 2.0f},
          {image_rect.origin.x + image_rect.extent.x - 2.0f, image_rect.origin.y + 2.0f},
          {image_rect.origin.x + image_rect.extent.x - 2.0f, image_rect.origin.y + image_rect.extent.y - 2.0f},
      }};
      preview_quad.color = resource ? resource->tint : std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f};
      preview_quad.uv = resource ? std::array<float, 4>{resource->uv.origin.x, resource->uv.origin.y, resource->uv.origin.x + resource->uv.extent.x,
                                                        resource->uv.origin.y + resource->uv.extent.y}
                                 : std::array<float, 4>{0.0f, 0.0f, 1.0f, 1.0f};
      preview_quad.texture_key = resource ? resource->texture_key : attr_string(node, "texture");
      preview_quad.clip_rect = clip_rect;
      scene.quads.push_back(std::move(preview_quad));
      if (!node.label.empty()) {
        label(scene, node.label,
              D2D1::RectF(image_rect.origin.x, image_rect.origin.y + image_rect.extent.y + 4.0f, image_rect.origin.x + image_rect.extent.x,
                          image_rect.origin.y + image_rect.extent.y + 22.0f),
              theme.text_secondary, TextStyle::body, clip_rect);
      }
      return;
    }
    case WidgetKind::progress_bar: {
      const float value = std::clamp(attr_float(node, "value").value_or(0.0f), 0.0f, 1.0f);
      label(scene, node.label,
            D2D1::RectF(area.origin.x, area.origin.y, area.origin.x + area.extent.x, area.origin.y + 18.0f), theme.text_secondary, TextStyle::body, clip_rect);
      quad(scene.quads, {{area.origin.x, area.origin.y + 22.0f}, {area.extent.x, 12.0f}}, theme.progress_track, clip_rect);
      quad(scene.quads, {{area.origin.x, area.origin.y + 22.0f}, {area.extent.x * value, 12.0f}}, theme.progress_fill, clip_rect);
      stroke_rect(scene.quads, {{area.origin.x, area.origin.y + 22.0f}, {area.extent.x, 12.0f}}, 1.0f, theme.separator, clip_rect);
      return;
    }
    case WidgetKind::separator:
      quad(scene.quads, {{area.origin.x, area.origin.y + 2.0f}, {area.extent.x, 2.0f}}, theme.separator, clip_rect);
      return;
    case WidgetKind::custom_draw: {
      CustomDrawPrimitive primitive{};
      if (!parse_custom_draw_primitive(attr_string(node, "primitive"), &primitive)) {
        return;
      }

      const auto color = attr_color(node, "color_", theme.accent);
      if (primitive == CustomDrawPrimitive::fill_rect) {
        quad(scene.quads,
             {{area.origin.x + attr_float(node, "x").value_or(0.0f), area.origin.y + attr_float(node, "y").value_or(0.0f)},
              {attr_float(node, "width").value_or(0.0f), attr_float(node, "height").value_or(0.0f)}},
             color, clip_rect);
        return;
      }
      if (primitive == CustomDrawPrimitive::stroke_rect) {
        const Rect rect{
            .origin = {area.origin.x + attr_float(node, "x").value_or(0.0f), area.origin.y + attr_float(node, "y").value_or(0.0f)},
            .extent = {attr_float(node, "width").value_or(0.0f), attr_float(node, "height").value_or(0.0f)},
        };
        const float thickness = attr_float(node, "thickness").value_or(1.0f);
        quad(scene.quads, {{rect.origin.x, rect.origin.y}, {rect.extent.x, thickness}}, color, clip_rect);
        quad(scene.quads, {{rect.origin.x, rect.origin.y + rect.extent.y - thickness}, {rect.extent.x, thickness}}, color, clip_rect);
        quad(scene.quads, {{rect.origin.x, rect.origin.y}, {thickness, rect.extent.y}}, color, clip_rect);
        quad(scene.quads, {{rect.origin.x + rect.extent.x - thickness, rect.origin.y}, {thickness, rect.extent.y}}, color, clip_rect);
        return;
      }
      if (primitive == CustomDrawPrimitive::line) {
        const Vec2 from{area.origin.x + attr_float(node, "x1").value_or(0.0f), area.origin.y + attr_float(node, "y1").value_or(0.0f)};
        const Vec2 to{area.origin.x + attr_float(node, "x2").value_or(0.0f), area.origin.y + attr_float(node, "y2").value_or(0.0f)};
        line_quad(scene.quads, from, to, attr_float(node, "thickness").value_or(1.0f), color, clip_rect);
        return;
      }

      Quad shader_quad{};
      shader_quad.color = attr_color(node, "tint_", {1.0f, 1.0f, 1.0f, 1.0f});
      shader_quad.shader_key = attr_shader_key(node);
      shader_quad.params = attr_shader_params(node);
      shader_quad.clip_rect = clip_rect;

      if (primitive == CustomDrawPrimitive::shader_rect) {
        const Rect rect{
            .origin = {area.origin.x + attr_float(node, "x").value_or(0.0f), area.origin.y + attr_float(node, "y").value_or(0.0f)},
            .extent = {clamp_non_negative(attr_float(node, "width").value_or(0.0f)),
                       clamp_non_negative(attr_float(node, "height").value_or(0.0f))},
        };
        shader_quad.points = {{
            {rect.origin.x, rect.origin.y},
            {rect.origin.x, rect.origin.y + rect.extent.y},
            {rect.origin.x + rect.extent.x, rect.origin.y},
            {rect.origin.x + rect.extent.x, rect.origin.y + rect.extent.y},
        }};
        const std::string_view image_key = attr_image_key(node);
        const auto resource_it = image_resources.find(image_key);
        if (resource_it != image_resources.end()) {
          shader_quad.uv = {resource_it->second.uv.origin.x,
                            resource_it->second.uv.origin.y,
                            resource_it->second.uv.origin.x + resource_it->second.uv.extent.x,
                            resource_it->second.uv.origin.y + resource_it->second.uv.extent.y};
          shader_quad.texture_key = resource_it->second.texture_key;
          shader_quad.color = multiply_color(shader_quad.color, resource_it->second.tint);
        } else {
          shader_quad.texture_key = attr_string(node, "texture");
        }
        scene.quads.push_back(std::move(shader_quad));
        return;
      }

      if (primitive == CustomDrawPrimitive::shader_image) {
        const std::string_view image_key = attr_image_key(node);
        const ImageResourceDesc* resource = nullptr;
        const auto resource_it = image_resources.find(image_key);
        if (resource_it != image_resources.end()) {
          resource = &resource_it->second;
        }

        const float width =
            std::clamp(attr_float(node, "width").value_or(resource ? resource->size.x : area.extent.x), 0.0f, clamp_non_negative(area.extent.x));
        const float height = clamp_non_negative(attr_float(node, "height").value_or(resource ? resource->size.y : 72.0f));
        const Rect image_rect{{area.origin.x, area.origin.y}, {width, height}};

        quad(scene.quads, image_rect, theme.panel_background, clip_rect);
        quad(scene.quads, {{image_rect.origin.x, image_rect.origin.y}, {image_rect.extent.x, 2.0f}}, theme.accent, clip_rect);
        stroke_rect(scene.quads, image_rect, 1.0f, theme.separator, clip_rect);

        shader_quad.points = {{
            {image_rect.origin.x + 2.0f, image_rect.origin.y + 2.0f},
            {image_rect.origin.x + 2.0f, image_rect.origin.y + image_rect.extent.y - 2.0f},
            {image_rect.origin.x + image_rect.extent.x - 2.0f, image_rect.origin.y + 2.0f},
            {image_rect.origin.x + image_rect.extent.x - 2.0f, image_rect.origin.y + image_rect.extent.y - 2.0f},
        }};
        if (resource != nullptr) {
          shader_quad.uv = {resource->uv.origin.x, resource->uv.origin.y, resource->uv.origin.x + resource->uv.extent.x,
                            resource->uv.origin.y + resource->uv.extent.y};
          shader_quad.texture_key = resource->texture_key;
          shader_quad.color = multiply_color(shader_quad.color, resource->tint);
        } else {
          shader_quad.texture_key = attr_string(node, "texture");
        }
        scene.quads.push_back(std::move(shader_quad));

        const std::string_view caption = attr_string(node, "label");
        if (!caption.empty()) {
          label(scene,
                caption,
                D2D1::RectF(image_rect.origin.x, image_rect.origin.y + image_rect.extent.y + 4.0f, image_rect.origin.x + image_rect.extent.x,
                            image_rect.origin.y + image_rect.extent.y + 22.0f),
                theme.text_secondary,
                TextStyle::body,
                clip_rect);
        }
        return;
      }
      return;
    }
  }
}

void build_scene(const FrameDocument& document,
                 const Dx11Theme& theme,
                 const StringMap<ImageResourceDesc>& image_resources,
                 const StringMap<FontResourceDesc>& font_descriptors,
                 StringMap<std::wstring>& wide_text_cache,
                 Scene& scene) {
  scene.quads.clear();
  scene.labels.clear();
  scene.wide_text_cache = &wide_text_cache;
  scene.quads.reserve(std::max(scene.quads.capacity(), document.widget_count() * 4));
  scene.labels.reserve(std::max(scene.labels.capacity(), document.widget_count() * 2));
  for (const auto& root : document.roots) {
    emit(root, {}, theme, image_resources, font_descriptors, scene, std::nullopt);
  }
}

Status validate_document_textures(const WidgetNode& node,
                                  const StringMap<ComPtr<ID3D11ShaderResourceView>>& textures,
                                  const StringMap<ImageResourceDesc>& image_resources,
                                  const StringMap<ShaderResourceDesc>& shader_resources) {
  if (node.kind == WidgetKind::image) {
    std::string_view texture_key = attr_string(node, "texture");
    const std::string_view image_key = attr_image_key(node);
    if (!image_key.empty()) {
      const auto image_it = image_resources.find(image_key);
      if (image_it != image_resources.end()) {
        texture_key = image_it->second.texture_key;
      }
    }
    if (texture_key.empty()) {
      return Status::invalid_argument("Dx11Backend image widgets require a registered texture key.");
    }
    if (!textures.contains(texture_key)) {
      return Status::invalid_argument("Dx11Backend is missing the texture binding for image resource '" + std::string(texture_key) + "'.");
    }
  }

  if (node.kind == WidgetKind::custom_draw) {
    CustomDrawPrimitive primitive = CustomDrawPrimitive::fill_rect;
    if (parse_custom_draw_primitive(attr_string(node, "primitive"), &primitive) &&
        (primitive == CustomDrawPrimitive::shader_rect || primitive == CustomDrawPrimitive::shader_image)) {
      const std::string_view shader_key = attr_shader_key(node);
      if (shader_key.empty()) {
        return Status::invalid_argument("Dx11Backend shader primitives require a registered shader key.");
      }
      const auto shader_it = shader_resources.find(shader_key);
      if (shader_it == shader_resources.end()) {
        return Status::invalid_argument("Dx11Backend is missing the shader resource '" + std::string(shader_key) + "'.");
      }
      if (shader_it->second.samples_texture) {
        std::string_view texture_key = attr_string(node, "texture");
        const std::string_view image_key = attr_image_key(node);
        if (!image_key.empty()) {
          const auto image_it = image_resources.find(image_key);
          if (image_it != image_resources.end()) {
            texture_key = image_it->second.texture_key;
          }
        }
        if (texture_key.empty()) {
          return Status::invalid_argument("Dx11Backend shader primitives require a texture when the shader samples textures.");
        }
        if (!textures.contains(texture_key)) {
          return Status::invalid_argument("Dx11Backend is missing the texture binding for shader resource '" + std::string(texture_key) + "'.");
        }
      }
    }
  }

  for (const auto& child : node.children) {
    const Status child_status = validate_document_textures(child, textures, image_resources, shader_resources);
    if (!child_status) {
      return child_status;
    }
  }
  return Status::success();
}

Status validate_document_textures(const FrameDocument& document,
                                  const StringMap<ComPtr<ID3D11ShaderResourceView>>& textures,
                                  const StringMap<ImageResourceDesc>& image_resources,
                                  const StringMap<ShaderResourceDesc>& shader_resources) {
  for (const auto& root : document.roots) {
    const Status status = validate_document_textures(root, textures, image_resources, shader_resources);
    if (!status) {
      return status;
    }
  }
  return Status::success();
}

QuadVertex vtx(float x, float y, const std::array<float, 4>& color, float u, float v, ExtentU viewport) {
  const float w = static_cast<float>(std::max(viewport.width, 1u));
  const float h = static_cast<float>(std::max(viewport.height, 1u));
  return {{(x / w) * 2.0f - 1.0f, 1.0f - (y / h) * 2.0f}, {color[0], color[1], color[2], color[3]}, {u, v}};
}

void append_vertices(std::vector<QuadVertex>& out, const Quad& quad_item, ExtentU viewport) {
  const QuadVertex a = vtx(quad_item.points[0].x, quad_item.points[0].y, quad_item.color, quad_item.uv[0], quad_item.uv[1], viewport);
  const QuadVertex b = vtx(quad_item.points[1].x, quad_item.points[1].y, quad_item.color, quad_item.uv[0], quad_item.uv[3], viewport);
  const QuadVertex c = vtx(quad_item.points[2].x, quad_item.points[2].y, quad_item.color, quad_item.uv[2], quad_item.uv[1], viewport);
  const QuadVertex d = vtx(quad_item.points[3].x, quad_item.points[3].y, quad_item.color, quad_item.uv[2], quad_item.uv[3], viewport);
  out.push_back(a); out.push_back(b); out.push_back(c);
  out.push_back(c); out.push_back(b); out.push_back(d);
}

void build_batches(const std::vector<Quad>& quads, ExtentU viewport, std::vector<QuadVertex>& vertices, std::vector<DrawBatch>& batches) {
  vertices.clear();
  batches.clear();
  vertices.reserve(std::max(vertices.capacity(), quads.size() * 6));
  batches.reserve(std::max(batches.capacity(), quads.size()));

  for (const auto& quad_item : quads) {
    const std::size_t start = vertices.size();
    append_vertices(vertices, quad_item, viewport);
    const std::size_t count = vertices.size() - start;
    if (quad_item.shader_key.empty() &&
        !batches.empty() &&
        batches.back().shader_key.empty() &&
        batches.back().texture_key == quad_item.texture_key &&
        same_optional_rect(batches.back().clip_rect, quad_item.clip_rect)) {
      batches.back().vertex_count += count;
      continue;
    }

    batches.push_back({
        .start_vertex = start,
        .vertex_count = count,
        .texture_key = quad_item.texture_key,
        .shader_key = quad_item.shader_key,
        .tint = quad_item.color,
        .params = quad_item.params,
        .bounds = {
            .origin = {quad_item.points[0].x, quad_item.points[0].y},
            .extent = {quad_item.points[3].x - quad_item.points[0].x, quad_item.points[3].y - quad_item.points[0].y},
        },
        .clip_rect = quad_item.clip_rect,
    });
  }
}

D3D11_RECT scissor_from_clip(const std::optional<Rect>& clip_rect, ExtentU viewport) {
  if (!clip_rect.has_value()) {
    return {0, 0, static_cast<LONG>(viewport.width), static_cast<LONG>(viewport.height)};
  }

  const Rect clip = *clip_rect;
  const LONG left = static_cast<LONG>(std::clamp(clip.origin.x, 0.0f, static_cast<float>(viewport.width)));
  const LONG top = static_cast<LONG>(std::clamp(clip.origin.y, 0.0f, static_cast<float>(viewport.height)));
  const LONG right = static_cast<LONG>(std::clamp(clip.origin.x + clip.extent.x, 0.0f, static_cast<float>(viewport.width)));
  const LONG bottom = static_cast<LONG>(std::clamp(clip.origin.y + clip.extent.y, 0.0f, static_cast<float>(viewport.height)));
  return {left, top, std::max(left, right), std::max(top, bottom)};
}

D2D1_RECT_F d2d_rect_from_clip(const Rect& clip_rect) {
  return D2D1::RectF(
      clip_rect.origin.x,
      clip_rect.origin.y,
      clip_rect.origin.x + clip_rect.extent.x,
      clip_rect.origin.y + clip_rect.extent.y);
}

}  // namespace

struct Dx11ShaderProgram {
  ShaderResourceDesc descriptor;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11InputLayout> input_layout;
  bool has_custom_vertex{};
};

class Dx11Backend::Impl {
 public:
  ComPtr<ID3D11Device> owned_device;
  ComPtr<ID3D11DeviceContext> owned_context;
  ComPtr<IDXGISwapChain> owned_swap_chain;
  ID3D11Device* device{};
  ID3D11DeviceContext* context{};
  IDXGISwapChain* swap_chain{};
  ComPtr<ID3D11RenderTargetView> rtv;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> solid_ps;
  ComPtr<ID3D11PixelShader> textured_ps;
  ComPtr<ID3D11InputLayout> layout;
  ComPtr<ID3D11Buffer> vertex_buffer;
  ComPtr<ID3D11Buffer> shader_constant_buffer;
  ComPtr<ID3D11BlendState> alpha_blend;
  ComPtr<ID3D11BlendState> opaque_blend;
  ComPtr<ID3D11BlendState> additive_blend;
  ComPtr<ID3D11RasterizerState> rasterizer;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID2D1Factory> d2d_factory;
  ComPtr<ID2D1RenderTarget> d2d_target;
  ComPtr<ID2D1SolidColorBrush> brush;
  ComPtr<IDWriteFactory> dwrite_factory;
  ComPtr<IDWriteTextFormat> title_format;
  ComPtr<IDWriteTextFormat> body_format;
  ComPtr<IDWriteTextFormat> center_format;
  StringMap<FontResourceDesc> font_descriptors;
  StringMap<ComPtr<IDWriteTextFormat>> font_formats;
  StringMap<std::wstring> wide_text_cache;
  StringMap<ImageResourceDesc> image_resources;
  StringMap<ShaderResourceDesc> shader_descriptors;
  StringMap<Dx11CompiledShader> shaders;
  StringMap<ComPtr<ID3D11ShaderResourceView>> textures;
  BackendFrameStats stats{};
  BackendTelemetrySnapshot telemetry{};
  StringMap<ScopeAccumulator> scope_totals;
  std::uint64_t telemetry_refresh_count{};
  std::size_t vertex_capacity{};
  Scene scratch_scene;
  std::vector<QuadVertex> scratch_vertices;
  std::vector<DrawBatch> scratch_batches;
};

DWRITE_FONT_WEIGHT to_dwrite_weight(FontWeight weight) noexcept {
  switch (weight) {
    case FontWeight::regular:
      return DWRITE_FONT_WEIGHT_NORMAL;
    case FontWeight::medium:
      return DWRITE_FONT_WEIGHT_MEDIUM;
    case FontWeight::semibold:
      return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    case FontWeight::bold:
      return DWRITE_FONT_WEIGHT_BOLD;
  }
  return DWRITE_FONT_WEIGHT_NORMAL;
}

DWRITE_FONT_STYLE to_dwrite_style(FontStyle style) noexcept {
  switch (style) {
    case FontStyle::normal:
      return DWRITE_FONT_STYLE_NORMAL;
    case FontStyle::italic:
      return DWRITE_FONT_STYLE_ITALIC;
  }
  return DWRITE_FONT_STYLE_NORMAL;
}

struct ContextStateBackup {
  UINT viewport_count{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
  D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
  UINT scissor_rect_count{D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE};
  D3D11_RECT scissor_rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
  ComPtr<ID3D11RenderTargetView> render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
  ComPtr<ID3D11DepthStencilView> depth_stencil_view;
  ComPtr<ID3D11BlendState> blend_state;
  FLOAT blend_factor[4]{};
  UINT sample_mask{};
  ComPtr<ID3D11RasterizerState> rasterizer_state;
  ComPtr<ID3D11InputLayout> input_layout;
  D3D11_PRIMITIVE_TOPOLOGY primitive_topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
  ComPtr<ID3D11Buffer> vertex_buffer;
  UINT vertex_stride{};
  UINT vertex_offset{};
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11ShaderResourceView> pixel_shader_resource;
  ComPtr<ID3D11SamplerState> pixel_shader_sampler;
  ComPtr<ID3D11Buffer> vertex_constant_buffer;
  ComPtr<ID3D11Buffer> pixel_constant_buffer;
  UINT vertex_shader_class_instance_count{D3D11_SHADER_MAX_INTERFACES};
  UINT pixel_shader_class_instance_count{D3D11_SHADER_MAX_INTERFACES};
  ComPtr<ID3D11ClassInstance> vertex_shader_class_instances[D3D11_SHADER_MAX_INTERFACES];
  ComPtr<ID3D11ClassInstance> pixel_shader_class_instances[D3D11_SHADER_MAX_INTERFACES];

  void capture(ID3D11DeviceContext* context) {
    context->RSGetViewports(&viewport_count, viewports);
    context->RSGetScissorRects(&scissor_rect_count, scissor_rects);

    ID3D11RenderTargetView* raw_render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* raw_depth_stencil_view{};
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw_render_targets, &raw_depth_stencil_view);
    for (std::size_t index = 0; index < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
      render_targets[index].Attach(raw_render_targets[index]);
    }
    depth_stencil_view.Attach(raw_depth_stencil_view);

    ID3D11BlendState* raw_blend_state{};
    context->OMGetBlendState(&raw_blend_state, blend_factor, &sample_mask);
    blend_state.Attach(raw_blend_state);

    ID3D11RasterizerState* raw_rasterizer_state{};
    context->RSGetState(&raw_rasterizer_state);
    rasterizer_state.Attach(raw_rasterizer_state);

    ID3D11InputLayout* raw_input_layout{};
    context->IAGetInputLayout(&raw_input_layout);
    input_layout.Attach(raw_input_layout);
    context->IAGetPrimitiveTopology(&primitive_topology);

    ID3D11Buffer* raw_vertex_buffer{};
    context->IAGetVertexBuffers(0, 1, &raw_vertex_buffer, &vertex_stride, &vertex_offset);
    vertex_buffer.Attach(raw_vertex_buffer);

    ID3D11VertexShader* raw_vertex_shader{};
    ID3D11ClassInstance* raw_vs_instances[D3D11_SHADER_MAX_INTERFACES]{};
    context->VSGetShader(&raw_vertex_shader, raw_vs_instances, &vertex_shader_class_instance_count);
    vertex_shader.Attach(raw_vertex_shader);
    for (UINT index = 0; index < vertex_shader_class_instance_count; ++index) {
      vertex_shader_class_instances[index].Attach(raw_vs_instances[index]);
    }

    ID3D11PixelShader* raw_pixel_shader{};
    ID3D11ClassInstance* raw_ps_instances[D3D11_SHADER_MAX_INTERFACES]{};
    context->PSGetShader(&raw_pixel_shader, raw_ps_instances, &pixel_shader_class_instance_count);
    pixel_shader.Attach(raw_pixel_shader);
    for (UINT index = 0; index < pixel_shader_class_instance_count; ++index) {
      pixel_shader_class_instances[index].Attach(raw_ps_instances[index]);
    }

    ID3D11ShaderResourceView* raw_shader_resource{};
    context->PSGetShaderResources(0, 1, &raw_shader_resource);
    pixel_shader_resource.Attach(raw_shader_resource);

    ID3D11SamplerState* raw_sampler{};
    context->PSGetSamplers(0, 1, &raw_sampler);
    pixel_shader_sampler.Attach(raw_sampler);

    ID3D11Buffer* raw_vs_constant_buffer{};
    context->VSGetConstantBuffers(0, 1, &raw_vs_constant_buffer);
    vertex_constant_buffer.Attach(raw_vs_constant_buffer);

    ID3D11Buffer* raw_ps_constant_buffer{};
    context->PSGetConstantBuffers(0, 1, &raw_ps_constant_buffer);
    pixel_constant_buffer.Attach(raw_ps_constant_buffer);
  }

  void restore(ID3D11DeviceContext* context) const {
    ID3D11RenderTargetView* raw_render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    for (std::size_t index = 0; index < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++index) {
      raw_render_targets[index] = render_targets[index].Get();
    }
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, raw_render_targets, depth_stencil_view.Get());
    if (viewport_count > 0) {
      context->RSSetViewports(viewport_count, viewports);
    }
    if (scissor_rect_count > 0) {
      context->RSSetScissorRects(scissor_rect_count, scissor_rects);
    }
    context->OMSetBlendState(blend_state.Get(), blend_factor, sample_mask);
    context->RSSetState(rasterizer_state.Get());
    context->IASetInputLayout(input_layout.Get());
    context->IASetPrimitiveTopology(primitive_topology);
    ID3D11Buffer* raw_vertex_buffer = vertex_buffer.Get();
    context->IASetVertexBuffers(0, 1, &raw_vertex_buffer, &vertex_stride, &vertex_offset);

    ID3D11ClassInstance* raw_vs_instances[D3D11_SHADER_MAX_INTERFACES]{};
    for (UINT index = 0; index < vertex_shader_class_instance_count; ++index) {
      raw_vs_instances[index] = vertex_shader_class_instances[index].Get();
    }
    context->VSSetShader(vertex_shader.Get(), raw_vs_instances, vertex_shader_class_instance_count);

    ID3D11ClassInstance* raw_ps_instances[D3D11_SHADER_MAX_INTERFACES]{};
    for (UINT index = 0; index < pixel_shader_class_instance_count; ++index) {
      raw_ps_instances[index] = pixel_shader_class_instances[index].Get();
    }
    context->PSSetShader(pixel_shader.Get(), raw_ps_instances, pixel_shader_class_instance_count);

    ID3D11ShaderResourceView* raw_shader_resource = pixel_shader_resource.Get();
    context->PSSetShaderResources(0, 1, &raw_shader_resource);

    ID3D11SamplerState* raw_sampler = pixel_shader_sampler.Get();
    context->PSSetSamplers(0, 1, &raw_sampler);

    ID3D11Buffer* raw_vs_constant_buffer = vertex_constant_buffer.Get();
    context->VSSetConstantBuffers(0, 1, &raw_vs_constant_buffer);

    ID3D11Buffer* raw_ps_constant_buffer = pixel_constant_buffer.Get();
    context->PSSetConstantBuffers(0, 1, &raw_ps_constant_buffer);
  }
};

class ScopedContextStateRestore {
 public:
  ScopedContextStateRestore(ID3D11DeviceContext* context, bool enabled)
      : context_(context), enabled_(enabled) {
    if (enabled_ && context_ != nullptr) {
      backup_.capture(context_);
    }
  }

  ~ScopedContextStateRestore() {
    if (enabled_ && context_ != nullptr) {
      backup_.restore(context_);
    }
  }

 private:
  ID3D11DeviceContext* context_{};
  bool enabled_{false};
  ContextStateBackup backup_{};
};

Status Dx11Backend::rebuild_render_targets() {
  if (!impl_ || impl_->swap_chain == nullptr || impl_->device == nullptr || impl_->d2d_factory == nullptr) {
    return Status::not_ready("Dx11Backend cannot rebuild render targets before the swap chain, device, and factories are ready.");
  }

  impl_->rtv.Reset();
  impl_->d2d_target.Reset();
  impl_->brush.Reset();

  if (viewport_.width == 0 || viewport_.height == 0) {
    return Status::success();
  }

  ComPtr<ID3D11Texture2D> back_buffer;
  HRESULT hr = impl_->swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
  if (FAILED(hr)) {
    return Status::backend_error("Dx11Backend failed to access the swap chain back buffer: " + hex_hr(hr));
  }

  hr = impl_->device->CreateRenderTargetView(back_buffer.Get(), nullptr, &impl_->rtv);
  if (FAILED(hr)) {
    return Status::backend_error("Dx11Backend failed to create the render target view: " + hex_hr(hr));
  }

  ComPtr<IDXGISurface> surface;
  hr = back_buffer.As(&surface);
  if (FAILED(hr)) {
    return Status::backend_error("Dx11Backend failed to access the DXGI surface: " + hex_hr(hr));
  }

  const auto props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED));
  hr = impl_->d2d_factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &impl_->d2d_target);
  if (FAILED(hr)) {
    return Status::backend_error("Dx11Backend failed to create the Direct2D render target: " + hex_hr(hr));
  }

  hr = impl_->d2d_target->CreateSolidColorBrush(to_d2d_color(config_.theme.text_primary), &impl_->brush);
  if (FAILED(hr)) {
    return Status::backend_error("Dx11Backend failed to create the Direct2D brush: " + hex_hr(hr));
  }

  impl_->d2d_target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  impl_->d2d_target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  return Status::success();
}

Status Dx11Backend::create_factories() {
  if (!impl_->d2d_factory) {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, impl_->d2d_factory.GetAddressOf());
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to create the Direct2D factory: " + hex_hr(hr));
    }
  }
  if (!impl_->dwrite_factory) {
    HRESULT hr =
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(impl_->dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to create the DirectWrite factory: " + hex_hr(hr));
    }
  }
  if (!impl_->title_format) {
    HRESULT hr = impl_->dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                         DWRITE_FONT_STRETCH_NORMAL, config_.theme.title_text_size, L"en-us", &impl_->title_format);
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to create the title text format: " + hex_hr(hr));
    }
    impl_->title_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    impl_->title_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  if (!impl_->body_format) {
    HRESULT hr = impl_->dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                         DWRITE_FONT_STRETCH_NORMAL, config_.theme.body_text_size, L"en-us", &impl_->body_format);
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to create the body text format: " + hex_hr(hr));
    }
    impl_->body_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    impl_->body_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    impl_->body_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  }
  if (!impl_->center_format) {
    HRESULT hr = impl_->dwrite_factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                                         DWRITE_FONT_STRETCH_NORMAL, config_.theme.body_text_size, L"en-us", &impl_->center_format);
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to create the center text format: " + hex_hr(hr));
    }
    impl_->center_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    impl_->center_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    impl_->center_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  return realize_registered_fonts();
}

Status Dx11Backend::realize_registered_fonts() {
  if (!impl_->dwrite_factory) {
    return Status::success();
  }
  for (const auto& [key, descriptor] : impl_->font_descriptors) {
    if (impl_->font_formats.contains(key)) {
      continue;
    }
    ComPtr<IDWriteTextFormat> format;
    const std::wstring& family = descriptor.family.empty() ? cache_wide_text(impl_->wide_text_cache, "Segoe UI")
                                                           : cache_wide_text(impl_->wide_text_cache, descriptor.family);
    const std::wstring& locale = descriptor.locale.empty() ? cache_wide_text(impl_->wide_text_cache, "en-us")
                                                           : cache_wide_text(impl_->wide_text_cache, descriptor.locale);
    HRESULT hr = impl_->dwrite_factory->CreateTextFormat(
        family.c_str(), nullptr, to_dwrite_weight(descriptor.weight), to_dwrite_style(descriptor.style),
        DWRITE_FONT_STRETCH_NORMAL, descriptor.size > 0.0f ? descriptor.size : config_.theme.body_text_size,
        locale.c_str(), &format);
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to create a registered text format: " + hex_hr(hr));
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    impl_->font_formats[key] = std::move(format);
  }
  return Status::success();
}

Status Dx11Backend::realize_registered_shaders() {
  if (impl_->device == nullptr) {
    return Status::success();
  }

  static constexpr D3D11_INPUT_ELEMENT_DESC kInputLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };

  for (const auto& [key, descriptor] : impl_->shader_descriptors) {
    if (impl_->shaders.contains(key)) {
      continue;
    }

    shaders::CompiledProgram compiled_program;
    Status status = shaders::compile_program(descriptor, {.debug_info = config_.enable_debug_layer}, &compiled_program);
    if (!status) {
      return Status::backend_error("Dx11Backend failed to compile shader '" + key + "': " + status.message());
    }

    Dx11CompiledShader runtime_shader;
    runtime_shader.descriptor = descriptor;

    HRESULT hr = impl_->device->CreatePixelShader(
        compiled_program.pixel.bytecode.data(),
        compiled_program.pixel.bytecode.size(),
        nullptr,
        &runtime_shader.pixel_shader);
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to create pixel shader '" + key + "': " + hex_hr(hr));
    }

    if (compiled_program.has_custom_vertex) {
      hr = impl_->device->CreateVertexShader(
          compiled_program.vertex.bytecode.data(),
          compiled_program.vertex.bytecode.size(),
          nullptr,
          &runtime_shader.vertex_shader);
      if (FAILED(hr)) {
        return Status::backend_error("Dx11Backend failed to create vertex shader '" + key + "': " + hex_hr(hr));
      }

      hr = impl_->device->CreateInputLayout(
          kInputLayout,
          static_cast<UINT>(std::size(kInputLayout)),
          compiled_program.vertex.bytecode.data(),
          compiled_program.vertex.bytecode.size(),
          &runtime_shader.input_layout);
      if (FAILED(hr)) {
        return Status::backend_error("Dx11Backend failed to create input layout for shader '" + key + "': " + hex_hr(hr));
      }
    }

    impl_->shaders[key] = std::move(runtime_shader);
  }

  return Status::success();
}

Status Dx11Backend::create_pipeline() {
  static constexpr char kVs[] =
      "struct VSInput{float2 position:POSITION;float4 color:COLOR;float2 uv:TEXCOORD0;};"
      "struct VSOutput{float4 position:SV_POSITION;float4 color:COLOR;float2 uv:TEXCOORD0;};"
      "VSOutput main(VSInput i){VSOutput o;o.position=float4(i.position,0,1);o.color=i.color;o.uv=i.uv;return o;}";
  static constexpr char kSolidPs[] =
      "struct PSInput{float4 position:SV_POSITION;float4 color:COLOR;float2 uv:TEXCOORD0;};"
      "float4 main(PSInput i):SV_TARGET{return i.color;}";
  static constexpr char kTexturedPs[] =
      "Texture2D texture0:register(t0);"
      "SamplerState sampler0:register(s0);"
      "struct PSInput{float4 position:SV_POSITION;float4 color:COLOR;float2 uv:TEXCOORD0;};"
      "float4 main(PSInput i):SV_TARGET{return texture0.Sample(sampler0, i.uv) * i.color;}";
  UINT flags = 0;
#if defined(_DEBUG)
  flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
  ComPtr<ID3DBlob> vs_blob;
  ComPtr<ID3DBlob> solid_ps_blob;
  ComPtr<ID3DBlob> textured_ps_blob;
  ComPtr<ID3DBlob> err_blob;
  HRESULT hr = D3DCompile(kVs, std::strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_5_0", flags, 0, &vs_blob, &err_blob);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to compile the vertex shader.");
  hr = D3DCompile(kSolidPs, std::strlen(kSolidPs), nullptr, nullptr, nullptr, "main", "ps_5_0", flags, 0, &solid_ps_blob, &err_blob);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to compile the solid pixel shader.");
  hr = D3DCompile(kTexturedPs, std::strlen(kTexturedPs), nullptr, nullptr, nullptr, "main", "ps_5_0", flags, 0, &textured_ps_blob, &err_blob);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to compile the textured pixel shader.");
  hr = impl_->device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &impl_->vs);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the vertex shader: " + hex_hr(hr));
  hr = impl_->device->CreatePixelShader(solid_ps_blob->GetBufferPointer(), solid_ps_blob->GetBufferSize(), nullptr, &impl_->solid_ps);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the solid pixel shader: " + hex_hr(hr));
  hr = impl_->device->CreatePixelShader(textured_ps_blob->GetBufferPointer(), textured_ps_blob->GetBufferSize(), nullptr, &impl_->textured_ps);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the textured pixel shader: " + hex_hr(hr));
  const D3D11_INPUT_ELEMENT_DESC desc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  hr = impl_->device->CreateInputLayout(desc, static_cast<UINT>(std::size(desc)), vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &impl_->layout);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the input layout: " + hex_hr(hr));
  D3D11_BLEND_DESC blend_desc{};
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  hr = impl_->device->CreateBlendState(&blend_desc, &impl_->alpha_blend);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the alpha blend state: " + hex_hr(hr));
  blend_desc.RenderTarget[0].BlendEnable = FALSE;
  hr = impl_->device->CreateBlendState(&blend_desc, &impl_->opaque_blend);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the opaque blend state: " + hex_hr(hr));
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  hr = impl_->device->CreateBlendState(&blend_desc, &impl_->additive_blend);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the additive blend state: " + hex_hr(hr));
  D3D11_RASTERIZER_DESC raster{};
  raster.FillMode = D3D11_FILL_SOLID;
  raster.CullMode = D3D11_CULL_NONE;
  raster.DepthClipEnable = TRUE;
  raster.ScissorEnable = TRUE;
  hr = impl_->device->CreateRasterizerState(&raster, &impl_->rasterizer);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the rasterizer state: " + hex_hr(hr));
  D3D11_SAMPLER_DESC sampler_desc{};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MinLOD = 0;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  hr = impl_->device->CreateSamplerState(&sampler_desc, &impl_->sampler);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the texture sampler: " + hex_hr(hr));
  D3D11_BUFFER_DESC constant_buffer_desc{};
  constant_buffer_desc.ByteWidth = sizeof(ShaderConstants);
  constant_buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  constant_buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  constant_buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  hr = impl_->device->CreateBuffer(&constant_buffer_desc, nullptr, &impl_->shader_constant_buffer);
  if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the shader constant buffer: " + hex_hr(hr));
  const Status shader_status = realize_registered_shaders();
  if (!shader_status) {
    return shader_status;
  }
  return Status::success();
}

void Dx11Backend::reset_device_objects(bool clear_textures) noexcept {
  invalidate_back_buffer_resources();
  impl_->font_formats.clear();
  impl_->shaders.clear();
  if (clear_textures) {
    impl_->textures.clear();
    detail::release_storage(impl_->textures);
  }
  impl_->vertex_buffer.Reset();
  impl_->shader_constant_buffer.Reset();
  impl_->vertex_capacity = 0;
  impl_->layout.Reset();
  impl_->vs.Reset();
  impl_->solid_ps.Reset();
  impl_->textured_ps.Reset();
  impl_->alpha_blend.Reset();
  impl_->opaque_blend.Reset();
  impl_->additive_blend.Reset();
  impl_->rasterizer.Reset();
  impl_->sampler.Reset();
  release_transient_storage();
}

Dx11Backend::Dx11Backend(Dx11BackendConfig config)
    : config_(config), viewport_(config.initial_viewport), impl_(std::make_unique<Impl>()) {}

Dx11Backend::~Dx11Backend() = default;

BackendKind Dx11Backend::kind() const noexcept { return BackendKind::dx11; }
std::string_view Dx11Backend::name() const noexcept { return "DirectX 11"; }

BackendCapabilities Dx11Backend::capabilities() const noexcept {
  return {
      .debug_layer = config_.enable_debug_layer,
      .user_textures = true,
      .user_shaders = true,
      .docking = true,
      .multi_viewport = false,
      .manual_host_binding = true,
      .host_state_restore = true,
      .injected_overlay = true,
  };
}

Status Dx11Backend::initialize() {
  const auto initialize_started = std::chrono::steady_clock::now();
  if (initialized_) {
    return Status::success();
  }
  const bool has_external_swap_chain = config_.swap_chain != nullptr;
  const bool has_any_external_resources = has_external_swap_chain || config_.device != nullptr || config_.device_context != nullptr;
  if (!has_external_swap_chain && has_any_external_resources) {
    return Status::invalid_argument("Dx11Backend external host mode requires a swap chain.");
  }
  if (!has_external_swap_chain && config_.window_handle == nullptr) {
    return Status::invalid_argument("Dx11Backend requires a valid HWND when it owns window and swap-chain creation.");
  }

  if (has_external_swap_chain) {
    impl_->swap_chain = config_.swap_chain;

    if (config_.device != nullptr) {
      impl_->device = config_.device;
    } else if (config_.derive_device_from_swap_chain) {
      HRESULT hr = impl_->swap_chain->GetDevice(IID_PPV_ARGS(&impl_->owned_device));
      if (FAILED(hr)) {
        return Status::backend_error("Dx11Backend failed to derive the D3D11 device from the swap chain: " + hex_hr(hr));
      }
      impl_->device = impl_->owned_device.Get();
    } else {
      return Status::invalid_argument("Dx11Backend requires a device or derive_device_from_swap_chain=true when attaching to an external swap chain.");
    }

    if (config_.device_context != nullptr) {
      impl_->context = config_.device_context;
    } else {
      impl_->device->GetImmediateContext(&impl_->owned_context);
      impl_->context = impl_->owned_context.Get();
      if (impl_->context == nullptr) {
        return Status::backend_error("Dx11Backend failed to acquire the D3D11 immediate context from the external device.");
      }
    }

    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    const HRESULT desc_hr = impl_->swap_chain->GetDesc(&swap_chain_desc);
    if (FAILED(desc_hr)) {
      return Status::backend_error("Dx11Backend failed to query the external swap chain description: " + hex_hr(desc_hr));
    }

    if (config_.window_handle == nullptr && config_.derive_window_from_swap_chain) {
      config_.window_handle = swap_chain_desc.OutputWindow;
    }

    if (viewport_.width == 0 || viewport_.height == 0) {
      viewport_.width = std::max(swap_chain_desc.BufferDesc.Width, 1u);
      viewport_.height = std::max(swap_chain_desc.BufferDesc.Height, 1u);
      if ((viewport_.width == 1 || viewport_.height == 1) && config_.window_handle != nullptr) {
        RECT r{};
        GetClientRect(config_.window_handle, &r);
        viewport_.width = std::max(static_cast<std::uint32_t>(r.right - r.left), 1u);
        viewport_.height = std::max(static_cast<std::uint32_t>(r.bottom - r.top), 1u);
      }
    }

    feature_level_ = impl_->device->GetFeatureLevel();
    owns_device_objects_ = false;
  } else {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (config_.enable_debug_layer ? D3D11_CREATE_DEVICE_DEBUG : 0u);
    if (viewport_.width == 0 || viewport_.height == 0) {
      RECT r{}; GetClientRect(config_.window_handle, &r);
      viewport_.width = std::max(static_cast<std::uint32_t>(r.right - r.left), 1u);
      viewport_.height = std::max(static_cast<std::uint32_t>(r.bottom - r.top), 1u);
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    DXGI_SWAP_CHAIN_DESC sc{}; sc.BufferCount = 1; sc.BufferDesc.Width = viewport_.width; sc.BufferDesc.Height = viewport_.height;
    sc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sc.OutputWindow = config_.window_handle;
    sc.SampleDesc.Count = 1; sc.Windowed = TRUE; sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const auto create_device = [&](D3D_DRIVER_TYPE driver, UINT local_flags) {
      impl_->owned_swap_chain.Reset(); impl_->owned_device.Reset(); impl_->owned_context.Reset();
      return D3D11CreateDeviceAndSwapChain(nullptr, driver, nullptr, local_flags, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &sc,
                                           &impl_->owned_swap_chain, &impl_->owned_device, &feature_level_, &impl_->owned_context);
    };
    HRESULT hr = create_device(D3D_DRIVER_TYPE_HARDWARE, flags);
    if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
      flags &= ~D3D11_CREATE_DEVICE_DEBUG; hr = create_device(D3D_DRIVER_TYPE_HARDWARE, flags);
    }
    if (FAILED(hr)) hr = create_device(D3D_DRIVER_TYPE_WARP, flags);
    if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the device and swap chain: " + hex_hr(hr));
    impl_->device = impl_->owned_device.Get(); impl_->context = impl_->owned_context.Get(); impl_->swap_chain = impl_->owned_swap_chain.Get();
    owns_device_objects_ = true;
  }

  Status status = create_factories(); if (!status) { shutdown(); return status; }
  status = rebuild_render_targets(); if (!status) { shutdown(); return status; }
  status = create_pipeline(); if (!status) { shutdown(); return status; }
  initialized_ = true;
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "initialize",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - initialize_started).count()));
    refresh_telemetry();
  }
  return Status::success();
}

void Dx11Backend::prepare_for_resize() noexcept {
  release_swap_chain_bindings(impl_ ? impl_->context : nullptr);
}

void Dx11Backend::invalidate_back_buffer_resources() noexcept {
  if (!impl_) {
    return;
  }
  prepare_for_resize();
  impl_->rtv.Reset();
  impl_->d2d_target.Reset();
  impl_->brush.Reset();
}

Status Dx11Backend::resize(ExtentU viewport) {
  const auto resize_started = std::chrono::steady_clock::now();
  if (!initialized_) return Status::not_ready("Dx11Backend must be initialized before resize.");
  if (!impl_ || impl_->swap_chain == nullptr || impl_->device == nullptr || impl_->context == nullptr) {
    return Status::not_ready("Dx11Backend cannot resize without an active swap chain, device, and device context.");
  }
  if (same_extent(viewport_, viewport) && impl_->rtv && impl_->d2d_target && impl_->brush) {
    if (config_.diagnostics.enabled) {
      record_scope(impl_->scope_totals,
                   "resize",
                   static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - resize_started).count()));
      refresh_telemetry();
    }
    return Status::success();
  }
  viewport_ = viewport;
  if (viewport_.width == 0 || viewport_.height == 0) {
    invalidate_back_buffer_resources();
    release_transient_storage();
    return Status::success();
  }
  if (config_.host.resize_mode == ResizeMode::backend_managed) {
    if (owns_device_objects_) {
      release_swap_chain_bindings(impl_->context);
    }
    invalidate_back_buffer_resources();
    const HRESULT hr = impl_->swap_chain->ResizeBuffers(0, viewport_.width, viewport_.height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
      return Status::backend_error(device_error_message(impl_->device, "Dx11Backend failed to resize swap chain buffers", hr));
    }
  }
  Status status = rebuild_render_targets();
  if (status && config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "resize",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - resize_started).count()));
    refresh_telemetry();
  }
  return status;
}

Status Dx11Backend::render(const FrameDocument& document) {
  const auto render_started = std::chrono::steady_clock::now();
  if (!initialized_) return Status::not_ready("Dx11Backend must be initialized before render.");
  if (!impl_ || impl_->swap_chain == nullptr || impl_->device == nullptr || impl_->context == nullptr) {
    return Status::not_ready("Dx11Backend cannot render without an active swap chain, device, and device context.");
  }
  impl_->stats = {};
  impl_->stats.frame_index = document.info.frame_index;
  last_widget_count_ = document.widget_count();
  if (viewport_.width == 0 || viewport_.height == 0) return Status::success();
  if (!impl_->rtv || !impl_->d2d_target || !impl_->brush) {
    const Status rebuild_status = rebuild_render_targets();
    if (!rebuild_status) {
      return rebuild_status;
    }
    if (!impl_->rtv || !impl_->d2d_target || !impl_->brush) {
      return Status::success();
    }
  }
  ScopedContextStateRestore state_restore(impl_->context, config_.host.restore_host_state);
  const Status texture_status = validate_document_textures(document, impl_->textures, impl_->image_resources, impl_->shader_descriptors);
  if (!texture_status) {
    return texture_status;
  }
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "validate_document",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - render_started).count()));
  }

  const auto build_started = std::chrono::steady_clock::now();
  build_scene(document, config_.theme, impl_->image_resources, impl_->font_descriptors, impl_->wide_text_cache, impl_->scratch_scene);
  build_batches(impl_->scratch_scene.quads, viewport_, impl_->scratch_vertices, impl_->scratch_batches);
  impl_->stats.label_count = impl_->scratch_scene.labels.size();
  impl_->stats.quad_count = impl_->scratch_scene.quads.size();
  impl_->stats.draw_batch_count = impl_->scratch_batches.size();
  impl_->stats.textured_batch_count =
      static_cast<std::size_t>(std::count_if(impl_->scratch_batches.begin(), impl_->scratch_batches.end(), [](const DrawBatch& batch) {
        return !batch.texture_key.empty();
      }));
  impl_->stats.shader_batch_count =
      static_cast<std::size_t>(std::count_if(impl_->scratch_batches.begin(), impl_->scratch_batches.end(), [](const DrawBatch& batch) {
        return !batch.shader_key.empty();
      }));
  impl_->stats.vertex_count = impl_->scratch_vertices.size();
  for (const auto& root : document.roots) {
    accumulate_widget_stats(root, impl_->stats);
  }
  impl_->stats.scene_build_microseconds =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - build_started).count());
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals, "scene_build", impl_->stats.scene_build_microseconds);
  }

  const auto upload_started = std::chrono::steady_clock::now();
  if (!impl_->scratch_vertices.empty()) {
    constexpr std::size_t kMaxVertexCount = static_cast<std::size_t>((std::numeric_limits<UINT>::max)() / sizeof(QuadVertex));
    if (impl_->scratch_vertices.size() > kMaxVertexCount) {
      return Status::invalid_argument("Dx11Backend vertex submission exceeds the D3D11 buffer size limit.");
    }
  }
  if (!impl_->scratch_vertices.empty() && impl_->scratch_vertices.size() > impl_->vertex_capacity) {
    impl_->vertex_buffer.Reset();
    std::size_t next_capacity = std::max<std::size_t>(impl_->vertex_capacity, 1024);
    while (next_capacity < impl_->scratch_vertices.size()) {
      next_capacity *= 2;
    }
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = static_cast<UINT>(next_capacity * sizeof(QuadVertex));
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    const HRESULT hr = impl_->device->CreateBuffer(&desc, nullptr, &impl_->vertex_buffer);
    if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to create the dynamic vertex buffer: " + hex_hr(hr));
    impl_->vertex_capacity = next_capacity;
  }
  if (!impl_->scratch_vertices.empty()) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = impl_->context->Map(impl_->vertex_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return Status::backend_error("Dx11Backend failed to map the dynamic vertex buffer: " + hex_hr(hr));
    std::memcpy(mapped.pData, impl_->scratch_vertices.data(), impl_->scratch_vertices.size() * sizeof(QuadVertex));
    impl_->context->Unmap(impl_->vertex_buffer.Get(), 0);
  }
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "vertex_upload",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - upload_started).count()));
  }

  const auto submit_started = std::chrono::steady_clock::now();
  const float blend[4] = {0, 0, 0, 0};
  const D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(viewport_.width), static_cast<float>(viewport_.height), 0.0f, 1.0f};
  impl_->context->OMSetRenderTargets(1, impl_->rtv.GetAddressOf(), nullptr);
  impl_->context->RSSetViewports(1, &vp);
  if (config_.host.clear_target) {
    impl_->context->ClearRenderTargetView(impl_->rtv.Get(), config_.clear_color.data());
  }
  impl_->context->RSSetState(impl_->rasterizer.Get());
  const D3D11_RECT full_scissor = scissor_from_clip(std::nullopt, viewport_);
  impl_->context->RSSetScissorRects(1, &full_scissor);
  impl_->context->OMSetBlendState(impl_->alpha_blend.Get(), blend, 0xFFFFFFFFu);
  if (!impl_->scratch_vertices.empty()) {
    const UINT stride = sizeof(QuadVertex), offset = 0;
    impl_->context->IASetInputLayout(impl_->layout.Get());
    impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    impl_->context->IASetVertexBuffers(0, 1, impl_->vertex_buffer.GetAddressOf(), &stride, &offset);
    impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = impl_->sampler.Get();
    impl_->context->PSSetSamplers(0, 1, &sampler);
    ID3D11Buffer* shader_constant_buffer = impl_->shader_constant_buffer.Get();
    impl_->context->VSSetConstantBuffers(0, 1, &shader_constant_buffer);
    impl_->context->PSSetConstantBuffers(0, 1, &shader_constant_buffer);
    const auto select_blend_state = [&](ShaderBlendMode blend_mode) -> ID3D11BlendState* {
      switch (blend_mode) {
        case ShaderBlendMode::opaque:
          return impl_->opaque_blend.Get();
        case ShaderBlendMode::additive:
          return impl_->additive_blend.Get();
        case ShaderBlendMode::alpha:
          return impl_->alpha_blend.Get();
      }
      return impl_->alpha_blend.Get();
    };
    for (const auto& batch : impl_->scratch_batches) {
      const D3D11_RECT scissor = scissor_from_clip(batch.clip_rect, viewport_);
      impl_->context->RSSetScissorRects(1, &scissor);
      if (batch.shader_key.empty() && batch.texture_key.empty()) {
        ID3D11ShaderResourceView* null_resource = nullptr;
        impl_->context->PSSetShaderResources(0, 1, &null_resource);
        impl_->context->IASetInputLayout(impl_->layout.Get());
        impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
        impl_->context->OMSetBlendState(impl_->alpha_blend.Get(), blend, 0xFFFFFFFFu);
        impl_->context->PSSetShader(impl_->solid_ps.Get(), nullptr, 0);
      } else if (batch.shader_key.empty()) {
        auto texture_it = impl_->textures.find(batch.texture_key);
        if (texture_it == impl_->textures.end()) {
          ID3D11ShaderResourceView* null_resource = nullptr;
          impl_->context->PSSetShaderResources(0, 1, &null_resource);
          impl_->context->IASetInputLayout(impl_->layout.Get());
          impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
          impl_->context->OMSetBlendState(impl_->alpha_blend.Get(), blend, 0xFFFFFFFFu);
          impl_->context->PSSetShader(impl_->solid_ps.Get(), nullptr, 0);
        } else {
          ID3D11ShaderResourceView* shader_resource = texture_it->second.Get();
          impl_->context->PSSetShaderResources(0, 1, &shader_resource);
          impl_->context->IASetInputLayout(impl_->layout.Get());
          impl_->context->VSSetShader(impl_->vs.Get(), nullptr, 0);
          impl_->context->OMSetBlendState(impl_->alpha_blend.Get(), blend, 0xFFFFFFFFu);
          impl_->context->PSSetShader(impl_->textured_ps.Get(), nullptr, 0);
        }
      } else {
        const auto shader_it = impl_->shaders.find(batch.shader_key);
        if (shader_it == impl_->shaders.end()) {
          return Status::invalid_argument("Dx11Backend is missing the realized shader '" + std::string(batch.shader_key) + "'.");
        }
        const Dx11CompiledShader& shader = shader_it->second;
        D3D11_MAPPED_SUBRESOURCE mapped_constants{};
        const HRESULT map_hr = impl_->context->Map(impl_->shader_constant_buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_constants);
        if (FAILED(map_hr)) {
          return Status::backend_error("Dx11Backend failed to map the shader constant buffer: " + hex_hr(map_hr));
        }
        ShaderConstants constants;
        constants.tint = batch.tint;
        constants.params = batch.params;
        constants.rect = {batch.bounds.origin.x, batch.bounds.origin.y, batch.bounds.extent.x, batch.bounds.extent.y};
        constants.viewport_and_time = {static_cast<float>(viewport_.width),
                                       static_cast<float>(viewport_.height),
                                       static_cast<float>(document.info.time_seconds),
                                       static_cast<float>(document.info.delta_seconds)};
        constants.frame_data = {static_cast<float>(document.info.frame_index), shader.descriptor.samples_texture ? 1.0f : 0.0f, 0.0f, 0.0f};
        std::memcpy(mapped_constants.pData, &constants, sizeof(constants));
        impl_->context->Unmap(impl_->shader_constant_buffer.Get(), 0);

        if (shader.input_layout) {
          impl_->context->IASetInputLayout(shader.input_layout.Get());
        } else {
          impl_->context->IASetInputLayout(impl_->layout.Get());
        }
        impl_->context->VSSetShader(shader.vertex_shader ? shader.vertex_shader.Get() : impl_->vs.Get(), nullptr, 0);
        impl_->context->OMSetBlendState(select_blend_state(shader.descriptor.blend_mode), blend, 0xFFFFFFFFu);
        if (shader.descriptor.samples_texture) {
          auto texture_it = impl_->textures.find(batch.texture_key);
          if (texture_it == impl_->textures.end()) {
            return Status::invalid_argument("Dx11Backend is missing the texture binding for shader batch '" + std::string(batch.texture_key) + "'.");
          }
          ID3D11ShaderResourceView* shader_resource = texture_it->second.Get();
          impl_->context->PSSetShaderResources(0, 1, &shader_resource);
        } else {
          ID3D11ShaderResourceView* null_resource = nullptr;
          impl_->context->PSSetShaderResources(0, 1, &null_resource);
        }
        impl_->context->PSSetShader(shader.pixel_shader.Get(), nullptr, 0);
      }

      impl_->context->Draw(static_cast<UINT>(batch.vertex_count), static_cast<UINT>(batch.start_vertex));
    }
  }
  impl_->d2d_target->BeginDraw();
  for (const auto& item : impl_->scratch_scene.labels) {
    impl_->brush->SetColor(to_d2d_color(item.color));
    IDWriteTextFormat* format = impl_->body_format.Get();
    if (!item.font_key.empty()) {
      const auto font_it = impl_->font_formats.find(item.font_key);
      if (font_it != impl_->font_formats.end()) {
        format = font_it->second.Get();
      }
    }
    if (format == impl_->body_format.Get()) {
      if (item.style == TextStyle::title) format = impl_->title_format.Get();
      if (item.style == TextStyle::center) format = impl_->center_format.Get();
    }
    if (item.clip_rect.has_value()) {
      impl_->d2d_target->PushAxisAlignedClip(d2d_rect_from_clip(*item.clip_rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    impl_->d2d_target->DrawText(item.text->c_str(), static_cast<UINT32>(item.text->size()), format, item.rect, impl_->brush.Get());
    if (item.clip_rect.has_value()) {
      impl_->d2d_target->PopAxisAlignedClip();
    }
  }
  const HRESULT draw_hr = impl_->d2d_target->EndDraw();
  if (draw_hr == D2DERR_RECREATE_TARGET) {
    invalidate_back_buffer_resources();
    return rebuild_render_targets();
  }
  if (FAILED(draw_hr)) {
    return Status::backend_error("Dx11Backend Direct2D text pass failed: " + hex_hr(draw_hr));
  }
  impl_->stats.render_submit_microseconds =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - submit_started).count());
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals, "render_submit", impl_->stats.render_submit_microseconds);
  }
  trim_wide_text_cache(impl_->wide_text_cache, config_.resource_budgets);
  trim_scratch_storage(impl_->scratch_scene, impl_->scratch_vertices, impl_->scratch_batches, config_.resource_budgets);
  trim_vertex_buffer(impl_->context, impl_->vertex_buffer, impl_->vertex_capacity, impl_->scratch_vertices.size(), config_.resource_budgets);
  refresh_telemetry();
  return Status::success();
}

Status Dx11Backend::present() {
  const auto present_started = std::chrono::steady_clock::now();
  if (!initialized_) return Status::not_ready("Dx11Backend must be initialized before present.");
  if (config_.host.presentation_mode == PresentationMode::host_managed) {
    return Status::success();
  }
  if (!impl_ || impl_->swap_chain == nullptr) {
    return Status::not_ready("Dx11Backend cannot present without an active swap chain.");
  }
  const HRESULT hr = impl_->swap_chain->Present(config_.enable_vsync ? 1u : 0u, 0u);
  if (hr == DXGI_STATUS_OCCLUDED || hr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS) return Status::success();
  if (FAILED(hr)) return Status::backend_error(device_error_message(impl_->device, "Dx11Backend failed to present the swap chain", hr));
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "present",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - present_started).count()));
    refresh_telemetry();
  }
  return Status::success();
}

BackendFrameStats Dx11Backend::frame_stats() const noexcept {
  return impl_ ? impl_->stats : BackendFrameStats{};
}

BackendTelemetrySnapshot Dx11Backend::telemetry() const noexcept {
  return impl_ ? impl_->telemetry : BackendTelemetrySnapshot{};
}

void Dx11Backend::refresh_telemetry() noexcept {
  if (!impl_) {
    return;
  }

  impl_->telemetry.frame = impl_->stats;
  if (!config_.diagnostics.enabled) {
    impl_->telemetry_refresh_count = 0;
    impl_->telemetry.process_memory = {};
    impl_->telemetry.gpu_memory = {};
    impl_->telemetry.resources = {};
    impl_->telemetry.scopes.clear();
    return;
  }

  ++impl_->telemetry_refresh_count;
  const std::uint32_t memory_interval = (std::max)(1u, config_.diagnostics.memory_sample_interval);
  const bool sample_memory = impl_->telemetry_refresh_count == 1 || (impl_->telemetry_refresh_count % memory_interval) == 0;

  if (config_.diagnostics.collect_process_memory && sample_memory) {
    impl_->telemetry.process_memory = query_process_memory();
  }
  if (config_.diagnostics.collect_gpu_memory && sample_memory) {
    ComPtr<IDXGIAdapter> adapter;
    impl_->telemetry.gpu_memory = query_gpu_memory(resolve_dxgi_adapter(impl_->device, adapter));
  }

  if (config_.diagnostics.collect_resource_usage) {
    ResourceUsageSnapshot resources{};
    resources.font_count = impl_->font_descriptors.size();
    resources.image_count = impl_->image_resources.size();
    resources.shader_count = impl_->shader_descriptors.size();
    resources.texture_count = impl_->textures.size();
    resources.font_cache_bytes = estimate_font_format_bytes(impl_->font_formats);
    resources.wide_text_cache_bytes = estimate_wide_text_cache_bytes(impl_->wide_text_cache);
    resources.scene_bytes = estimate_scene_bytes(impl_->scratch_scene);
    resources.scratch_vertex_bytes = static_cast<std::uint64_t>(impl_->scratch_vertices.capacity()) * sizeof(QuadVertex);
    resources.scratch_batch_bytes = static_cast<std::uint64_t>(impl_->scratch_batches.capacity()) * sizeof(DrawBatch);
    resources.gpu_vertex_buffer_bytes = estimate_d3d11_buffer_bytes(impl_->vertex_buffer.Get());
    resources.gpu_constant_buffer_bytes = estimate_d3d11_buffer_bytes(impl_->shader_constant_buffer.Get());
    resources.total_estimated_bytes = resources.font_cache_bytes + resources.wide_text_cache_bytes + resources.scene_bytes +
                                      resources.scratch_vertex_bytes + resources.scratch_batch_bytes +
                                      resources.gpu_vertex_buffer_bytes + resources.gpu_constant_buffer_bytes;
    impl_->telemetry.resources = resources;
  }

  if (config_.diagnostics.collect_scope_timings) {
    impl_->telemetry.scopes.clear();
    impl_->telemetry.scopes.reserve(impl_->scope_totals.size());
    for (const auto& [name, totals] : impl_->scope_totals) {
      impl_->telemetry.scopes.push_back({
          .name = name,
          .call_count = totals.call_count,
          .total_microseconds = totals.total_microseconds,
          .max_microseconds = totals.max_microseconds,
      });
    }
    std::sort(impl_->telemetry.scopes.begin(), impl_->telemetry.scopes.end(), [](const ScopeTelemetry& lhs, const ScopeTelemetry& rhs) {
      if (lhs.total_microseconds == rhs.total_microseconds) {
        return lhs.name < rhs.name;
      }
      return lhs.total_microseconds > rhs.total_microseconds;
    });
    if (impl_->telemetry.scopes.size() > config_.diagnostics.top_scope_limit) {
      impl_->telemetry.scopes.resize(config_.diagnostics.top_scope_limit);
    }
  }
}

Status Dx11Backend::register_font(std::string_view key, const FontResourceDesc& descriptor) {
  const auto started = std::chrono::steady_clock::now();
  if (key.empty()) {
    return Status::invalid_argument("Dx11Backend font registration requires a non-empty key.");
  }
  const Status validation_status = validate_font_descriptor("Dx11Backend", descriptor);
  if (!validation_status) {
    return validation_status;
  }
  impl_->font_descriptors[std::string(key)] = descriptor;
  impl_->font_formats.erase(std::string(key));
  Status status = realize_registered_fonts();
  if (status && config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "register_font",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
    refresh_telemetry();
  }
  return status;
}

void Dx11Backend::unregister_font(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->font_descriptors.erase(std::string(key));
  impl_->font_formats.erase(std::string(key));
  refresh_telemetry();
}

Status Dx11Backend::register_image(std::string_view key, const ImageResourceDesc& descriptor) {
  const auto started = std::chrono::steady_clock::now();
  if (key.empty()) {
    return Status::invalid_argument("Dx11Backend image registration requires a non-empty key.");
  }
  const Status validation_status = validate_image_descriptor("Dx11Backend", descriptor);
  if (!validation_status) {
    return validation_status;
  }
  impl_->image_resources[std::string(key)] = descriptor;
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "register_image",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
    refresh_telemetry();
  }
  return Status::success();
}

void Dx11Backend::unregister_image(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->image_resources.erase(std::string(key));
  refresh_telemetry();
}

Status Dx11Backend::register_shader(std::string_view key, const ShaderResourceDesc& descriptor) {
  const auto started = std::chrono::steady_clock::now();
  if (key.empty()) {
    return Status::invalid_argument("Dx11Backend shader registration requires a non-empty key.");
  }
  if (descriptor.pixel.source.empty()) {
    return Status::invalid_argument("Dx11Backend shader registration requires a pixel shader source.");
  }
  impl_->shader_descriptors[std::string(key)] = descriptor;
  impl_->shaders.erase(std::string(key));
  Status status = realize_registered_shaders();
  if (status && config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "register_shader",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - started).count()));
    refresh_telemetry();
  }
  return status;
}

void Dx11Backend::unregister_shader(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->shader_descriptors.erase(std::string(key));
  impl_->shaders.erase(std::string(key));
  refresh_telemetry();
}

Status Dx11Backend::register_texture(std::string_view key, const Dx11TextureBinding& binding) {
  if (key.empty()) {
    return Status::invalid_argument("Dx11Backend texture registration requires a non-empty key.");
  }
  if (binding.shader_resource_view == nullptr) {
    return Status::invalid_argument("Dx11Backend texture registration requires a valid shader resource view.");
  }

  if (!initialized_) {
    return Status::not_ready("Dx11Backend must be initialized before registering textures.");
  }
  if (impl_->device == nullptr) {
    return Status::not_ready("Dx11Backend cannot register textures before the device is ready.");
  }

  ComPtr<ID3D11Resource> resource;
  binding.shader_resource_view->GetResource(&resource);
  if (!resource) {
    return Status::invalid_argument("Dx11Backend texture registration requires a shader resource view with an underlying resource.");
  }

  ComPtr<ID3D11Device> texture_device;
  resource->GetDevice(&texture_device);
  if (!texture_device || texture_device.Get() != impl_->device) {
    return Status::invalid_argument("Dx11Backend texture registration requires a texture created by the active D3D11 device.");
  }

  ComPtr<ID3D11ShaderResourceView> texture_view;
  texture_view.Attach(binding.shader_resource_view);
  texture_view.Get()->AddRef();
  impl_->textures[std::string(key)] = std::move(texture_view);
  if (config_.diagnostics.enabled) {
    refresh_telemetry();
  }
  return Status::success();
}

Status Dx11Backend::register_texture(std::string_view key, ID3D11ShaderResourceView* shader_resource_view) {
  return register_texture(key, Dx11TextureBinding{shader_resource_view});
}

void Dx11Backend::unregister_texture(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->textures.erase(std::string(key));
  refresh_telemetry();
}

Status Dx11Backend::rebind_host(const Dx11HostBinding& binding) {
  if (config_.host.host_mode == HostMode::owned_window) {
    return Status::invalid_argument("Dx11Backend rebind_host is only valid for external or injected host modes.");
  }
  if (binding.swap_chain == nullptr) {
    return Status::invalid_argument("Dx11Backend host rebinding requires a swap chain.");
  }

  const ID3D11Device* previous_device = impl_->device;
  config_.window_handle = binding.window_handle;
  config_.device = binding.device;
  config_.device_context = binding.device_context;
  config_.swap_chain = binding.swap_chain;
  config_.derive_device_from_swap_chain = binding.derive_device_from_swap_chain;
  config_.derive_window_from_swap_chain = binding.derive_window_from_swap_chain;
  if (binding.viewport.width > 0 && binding.viewport.height > 0) {
    viewport_ = binding.viewport;
  }

  if (!initialized_) {
    return initialize();
  }

  reset_device_objects(previous_device != binding.device && binding.device != nullptr);
  impl_->owned_swap_chain.Reset();
  impl_->owned_context.Reset();
  impl_->owned_device.Reset();
  impl_->swap_chain = binding.swap_chain;

  if (config_.device != nullptr) {
    impl_->device = config_.device;
  } else if (config_.derive_device_from_swap_chain) {
    HRESULT hr = impl_->swap_chain->GetDevice(IID_PPV_ARGS(&impl_->owned_device));
    if (FAILED(hr)) {
      return Status::backend_error("Dx11Backend failed to derive the D3D11 device from the rebound swap chain: " + hex_hr(hr));
    }
    impl_->device = impl_->owned_device.Get();
  } else {
    return Status::invalid_argument("Dx11Backend rebinding requires a device or derive_device_from_swap_chain=true.");
  }

  if (config_.device_context != nullptr) {
    impl_->context = config_.device_context;
  } else {
    impl_->device->GetImmediateContext(&impl_->owned_context);
    impl_->context = impl_->owned_context.Get();
    if (impl_->context == nullptr) {
      return Status::backend_error("Dx11Backend failed to acquire the rebound immediate context.");
    }
  }

  DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
  const HRESULT desc_hr = impl_->swap_chain->GetDesc(&swap_chain_desc);
  if (FAILED(desc_hr)) {
    return Status::backend_error("Dx11Backend failed to query the rebound swap-chain description: " + hex_hr(desc_hr));
  }
  if (config_.window_handle == nullptr && config_.derive_window_from_swap_chain) {
    config_.window_handle = swap_chain_desc.OutputWindow;
  }
  if (viewport_.width == 0 || viewport_.height == 0) {
    viewport_.width = std::max(swap_chain_desc.BufferDesc.Width, 1u);
    viewport_.height = std::max(swap_chain_desc.BufferDesc.Height, 1u);
  }
  feature_level_ = impl_->device->GetFeatureLevel();
  Status status = create_factories();
  if (!status) {
    return status;
  }
  status = rebuild_render_targets();
  if (!status) {
    return status;
  }
  if (!impl_->vs || previous_device != impl_->device) {
    status = create_pipeline();
    if (!status) {
      return status;
    }
  }
  return Status::success();
}

void Dx11Backend::detach_host() noexcept {
  if (!impl_ || owns_device_objects_) {
    return;
  }
  reset_device_objects(true);
  impl_->swap_chain = nullptr;
  impl_->context = nullptr;
  impl_->device = nullptr;
  impl_->owned_swap_chain.Reset();
  impl_->owned_context.Reset();
  impl_->owned_device.Reset();
  config_.window_handle = nullptr;
  config_.device = nullptr;
  config_.device_context = nullptr;
  config_.swap_chain = nullptr;
  viewport_ = {};
  last_widget_count_ = 0;
  feature_level_ = D3D_FEATURE_LEVEL_11_0;
  initialized_ = false;
  release_transient_storage();
}

void Dx11Backend::shutdown() noexcept {
  if (impl_) {
    reset_device_objects(true);
    impl_->image_resources.clear();
    impl_->font_descriptors.clear();
    impl_->shader_descriptors.clear();
    detail::release_storage(impl_->image_resources);
    detail::release_storage(impl_->font_descriptors);
    detail::release_storage(impl_->shader_descriptors);
    detail::release_storage(impl_->font_formats);
    detail::release_storage(impl_->wide_text_cache);
    detail::release_storage(impl_->shaders);
    impl_->d2d_factory.Reset();
    impl_->title_format.Reset(); impl_->body_format.Reset(); impl_->center_format.Reset(); impl_->dwrite_factory.Reset();
    impl_->swap_chain = nullptr; impl_->context = nullptr; impl_->device = nullptr;
    impl_->owned_swap_chain.Reset(); impl_->owned_context.Reset(); impl_->owned_device.Reset(); impl_->vertex_capacity = 0;
    impl_->stats = {};
    impl_->telemetry = {};
    impl_->telemetry_refresh_count = 0;
  }
  viewport_ = {}; last_widget_count_ = 0; feature_level_ = D3D_FEATURE_LEVEL_11_0; initialized_ = false; owns_device_objects_ = false;
}

std::size_t Dx11Backend::last_widget_count() const noexcept { return last_widget_count_; }
bool Dx11Backend::initialized() const noexcept { return initialized_; }
bool Dx11Backend::owns_device_objects() const noexcept { return owns_device_objects_; }
bool Dx11Backend::has_font(std::string_view key) const noexcept { return impl_ && impl_->font_descriptors.contains(key); }
bool Dx11Backend::has_image(std::string_view key) const noexcept { return impl_ && impl_->image_resources.contains(key); }
bool Dx11Backend::has_shader(std::string_view key) const noexcept { return impl_ && impl_->shader_descriptors.contains(key); }
bool Dx11Backend::has_texture(std::string_view key) const noexcept { return impl_ && impl_->textures.contains(key); }
D3D_FEATURE_LEVEL Dx11Backend::feature_level() const noexcept { return feature_level_; }
HWND Dx11Backend::window_handle() const noexcept { return config_.window_handle; }
ID3D11Device* Dx11Backend::device_handle() const noexcept { return impl_ ? impl_->device : nullptr; }
ID3D11DeviceContext* Dx11Backend::device_context_handle() const noexcept { return impl_ ? impl_->context : nullptr; }
IDXGISwapChain* Dx11Backend::swap_chain_handle() const noexcept { return impl_ ? impl_->swap_chain : nullptr; }

void Dx11Backend::release_transient_storage() noexcept {
  if (!impl_) {
    return;
  }
  impl_->scratch_scene.quads.clear();
  impl_->scratch_scene.labels.clear();
  impl_->scratch_scene.wide_text_cache = nullptr;
  detail::release_storage(impl_->scratch_scene.quads);
  detail::release_storage(impl_->scratch_scene.labels);
  detail::release_storage(impl_->scratch_vertices);
  detail::release_storage(impl_->scratch_batches);
  detail::release_storage(impl_->wide_text_cache);
}

}  // namespace igr::backends


