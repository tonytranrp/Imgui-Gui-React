#include "igr/backends/dx12_backend.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <d2d1_3.h>
#include <d3d11.h>
#include <d3d11on12.h>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <dwrite.h>
#include <wrl/client.h>

#include "igr/detail/string_lookup.hpp"
#include "igr/shader_compiler.hpp"

namespace igr::backends {
namespace {

using Microsoft::WRL::ComPtr;
template <typename Value>
using StringMap = detail::TransparentStringMap<Value>;

constexpr UINT kFrameCount = 2;
constexpr DXGI_FORMAT kRenderTargetFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

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

constexpr std::string_view kInternalTextAtlasKey = "__igr_internal_text_atlas";

struct TextAtlasPlacement {
  const TextLabel* label{};
  std::uint32_t atlas_x{};
  std::uint32_t atlas_y{};
  std::uint32_t width{};
  std::uint32_t height{};
  Rect output_rect{};
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

struct Dx12CompiledShader {
  ShaderResourceDesc descriptor;
  ComPtr<ID3D12PipelineState> pipeline_state;
};

struct ScopeAccumulator {
  std::uint64_t call_count{};
  std::uint64_t total_microseconds{};
  std::uint64_t max_microseconds{};
};

using D3D11On12CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*,
                                                 UINT,
                                                 const D3D_FEATURE_LEVEL*,
                                                 UINT,
                                                 IUnknown* const*,
                                                 UINT,
                                                 UINT,
                                                 ID3D11Device**,
                                                 ID3D11DeviceContext**,
                                                 D3D_FEATURE_LEVEL*);
using D2D1CreateFactoryFn = HRESULT(WINAPI*)(D2D1_FACTORY_TYPE, REFIID, const D2D1_FACTORY_OPTIONS*, void**);
using DWriteCreateFactoryFn = HRESULT(WINAPI*)(DWRITE_FACTORY_TYPE, REFIID, IUnknown**);

struct Dx12TextInteropApi {
  HMODULE d3d11_module{};
  HMODULE d2d1_module{};
  HMODULE dwrite_module{};
  D3D11On12CreateDeviceFn create_d3d11on12_device{};
  D2D1CreateFactoryFn create_d2d1_factory{};
  DWriteCreateFactoryFn create_dwrite_factory{};
};

struct OwnedFrameResources {
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12Resource> back_buffer;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
  std::uint64_t fence_value{};
  ComPtr<ID3D11Resource> wrapped_back_buffer;
  ComPtr<ID2D1Bitmap1> text_bitmap;
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

std::string device_error_message(ID3D12Device* device, const char* prefix, HRESULT hr) {
  std::string message = std::string(prefix) + ": " + hex_hr(hr);
  if (is_device_lost(hr) && device != nullptr) {
    message += " (device removed reason: " + hex_hr(device->GetDeviceRemovedReason()) + ")";
  }
  return message;
}

std::string recent_debug_messages(ID3D12Device* device, std::uint64_t max_messages = 8) {
  if (device == nullptr) {
    return {};
  }
  ComPtr<ID3D12InfoQueue> info_queue;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info_queue))) || !info_queue) {
    return {};
  }

  const UINT64 total_messages = info_queue->GetNumStoredMessagesAllowedByRetrievalFilter();
  if (total_messages == 0) {
    return {};
  }

  const UINT64 start_index = total_messages > max_messages ? total_messages - max_messages : 0;
  std::ostringstream stream;
  for (UINT64 index = start_index; index < total_messages; ++index) {
    SIZE_T message_size = 0;
    if (FAILED(info_queue->GetMessage(index, nullptr, &message_size)) || message_size == 0) {
      continue;
    }
    std::vector<std::byte> storage(message_size);
    auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
    if (FAILED(info_queue->GetMessage(index, message, &message_size)) || message == nullptr) {
      continue;
    }
    if (stream.tellp() == std::streampos(0)) {
      stream << " [d3d12:";
    } else {
      stream << " | ";
    }
    stream << static_cast<int>(message->Severity) << ":" << static_cast<int>(message->ID) << " " << message->pDescription;
  }
  if (stream.tellp() != std::streampos(0)) {
    stream << "]";
  }
  return stream.str();
}

bool same_extent(ExtentU lhs, ExtentU rhs) noexcept {
  return lhs.width == rhs.width && lhs.height == rhs.height;
}

float clamp_non_negative(float value) {
  return std::max(0.0f, value);
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
  const auto cached = cache.find(text);
  if (cached != cache.end()) {
    return cached->second;
  }
  auto [inserted, _] = cache.emplace(std::string(text), wide(text));
  return inserted->second;
}

std::string_view attr_string(const WidgetNode& node, std::string_view name, std::string_view fallback = {}) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name == name) {
      return attribute.value;
    }
  }
  return fallback;
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

std::uint32_t bytes_per_pixel(DXGI_FORMAT format) noexcept {
  switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      return 4;
    default:
      return 0;
  }
}

std::uint64_t estimate_d3d12_resource_bytes(ID3D12Resource* resource) {
  if (resource == nullptr) {
    return 0;
  }
  const D3D12_RESOURCE_DESC desc = resource->GetDesc();
  if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
    return static_cast<std::uint64_t>(desc.Width);
  }

  const std::uint32_t bpp = bytes_per_pixel(desc.Format);
  if (bpp == 0) {
    return static_cast<std::uint64_t>(desc.Width);
  }

  std::uint64_t total = 0;
  std::uint64_t mip_width = desc.Width;
  std::uint32_t mip_height = desc.Height;
  for (std::uint16_t mip = 0; mip < (std::max)(desc.MipLevels, static_cast<std::uint16_t>(1)); ++mip) {
    total += mip_width * static_cast<std::uint64_t>(mip_height) * desc.DepthOrArraySize * bpp;
    mip_width = (std::max<std::uint64_t>)(1, mip_width >> 1u);
    mip_height = (std::max<std::uint32_t>)(1, mip_height >> 1u);
  }
  return total;
}

std::uint64_t estimate_bitmap_bytes(std::uint32_t width, std::uint32_t height) {
  return static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4u;
}

std::uint32_t next_power_of_two(std::uint32_t value) {
  std::uint32_t power = 1;
  while (power < value) {
    power <<= 1u;
  }
  return power;
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

void trim_upload_buffer(ComPtr<ID3D12Resource>& buffer,
                        std::byte*& mapped,
                        std::size_t& capacity,
                        std::size_t active_count,
                        std::size_t max_retained,
                        std::size_t minimum_capacity) {
  if (!buffer) {
    return;
  }
  if (max_retained == 0) {
    if (mapped != nullptr) {
      buffer->Unmap(0, nullptr);
      mapped = nullptr;
    }
    buffer.Reset();
    capacity = 0;
    return;
  }
  const std::size_t target_capacity = retained_capacity_target(active_count, max_retained, minimum_capacity);
  if (capacity <= target_capacity * 2) {
    return;
  }
  if (mapped != nullptr) {
    buffer->Unmap(0, nullptr);
    mapped = nullptr;
  }
  buffer.Reset();
  capacity = 0;
}

void fnv1a_append(std::uint64_t& hash, const void* bytes, std::size_t size) noexcept {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  const auto* cursor = static_cast<const std::uint8_t*>(bytes);
  for (std::size_t index = 0; index < size; ++index) {
    hash ^= cursor[index];
    hash *= kPrime;
  }
}

void fnv1a_append_u32(std::uint64_t& hash, std::uint32_t value) noexcept {
  fnv1a_append(hash, &value, sizeof(value));
}

void fnv1a_append_view(std::uint64_t& hash, std::wstring_view view) noexcept {
  fnv1a_append_u32(hash, static_cast<std::uint32_t>(view.size()));
  if (!view.empty()) {
    fnv1a_append(hash, view.data(), view.size() * sizeof(wchar_t));
  }
}

void fnv1a_append_view(std::uint64_t& hash, std::string_view view) noexcept {
  fnv1a_append_u32(hash, static_cast<std::uint32_t>(view.size()));
  if (!view.empty()) {
    fnv1a_append(hash, view.data(), view.size());
  }
}

std::uint64_t text_atlas_signature(const std::vector<TextAtlasPlacement>& placements) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  for (const auto& placement : placements) {
    const TextLabel* label = placement.label;
    if (label == nullptr || label->text == nullptr) {
      continue;
    }
    fnv1a_append_u32(hash, placement.width);
    fnv1a_append_u32(hash, placement.height);
    fnv1a_append_u32(hash, static_cast<std::uint32_t>(label->style));
    fnv1a_append_view(hash, *label->text);
    fnv1a_append_view(hash, label->font_key);
  }
  return hash;
}

IDXGIAdapter* resolve_dxgi_adapter(ID3D12Device* device,
                                   IDXGIFactory4* existing_factory,
                                   ComPtr<IDXGIFactory4>& owned_factory,
                                   ComPtr<IDXGIAdapter>& adapter) {
  if (device == nullptr) {
    return nullptr;
  }
  if (existing_factory != nullptr) {
    owned_factory = existing_factory;
  } else if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&owned_factory)))) {
    return nullptr;
  }
  const LUID luid = device->GetAdapterLuid();
  if (FAILED(owned_factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)))) {
    return nullptr;
  }
  return adapter.Get();
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

std::array<ShaderVector4, 4> attr_shader_params(const WidgetNode& node) {
  return {
      attr_color(node, "param0_", {0.0f, 0.0f, 0.0f, 0.0f}),
      attr_color(node, "param1_", {0.0f, 0.0f, 0.0f, 0.0f}),
      attr_color(node, "param2_", {0.0f, 0.0f, 0.0f, 0.0f}),
      attr_color(node, "param3_", {0.0f, 0.0f, 0.0f, 0.0f}),
  };
}

std::array<float, 4> multiply_color(const std::array<float, 4>& lhs, const std::array<float, 4>& rhs) {
  return {
      lhs[0] * rhs[0],
      lhs[1] * rhs[1],
      lhs[2] * rhs[2],
      lhs[3] * rhs[3],
  };
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
  return {.origin = {left, top}, .extent = {std::max(0.0f, right - left), std::max(0.0f, bottom - top)}};
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
  return lhs->origin.x == rhs->origin.x && lhs->origin.y == rhs->origin.y && lhs->extent.x == rhs->extent.x &&
         lhs->extent.y == rhs->extent.y;
}

float measure(const WidgetNode& node,
              const Dx12Theme& theme,
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
      .clip_rect = std::move(clip_rect),
  });
}

void emit(const WidgetNode& node,
          Rect area,
          const Dx12Theme& theme,
          const StringMap<ImageResourceDesc>& image_resources,
          const StringMap<FontResourceDesc>& font_descriptors,
          Scene& scene,
          const std::optional<Rect>& clip_rect);

void emit_stack(const WidgetNode& node,
                Rect area,
                const Dx12Theme& theme,
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
          const Dx12Theme& theme,
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
      quad(scene.quads,
           {{rect.origin.x + 10.0f, rect.origin.y + theme.window_title_height + 10.0f},
            {rect.extent.x - 20.0f, rect.extent.y - theme.window_title_height - 20.0f}},
           theme.panel_background);
      stroke_rect(scene.quads,
                  {{rect.origin.x + 10.0f, rect.origin.y + theme.window_title_height + 10.0f},
                   {rect.extent.x - 20.0f, rect.extent.y - theme.window_title_height - 20.0f}},
                  1.0f,
                  theme.separator);
      label(scene,
            node.label,
            D2D1::RectF(rect.origin.x + 14.0f, rect.origin.y + 2.0f, rect.origin.x + rect.extent.x - 14.0f, rect.origin.y + theme.window_title_height),
            theme.text_primary,
            TextStyle::title);
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
    case WidgetKind::text: {
      const auto font_it = font_descriptors.find(attr_string(node, "font"));
      const float text_height = font_it != font_descriptors.end() ? std::max(20.0f, font_it->second.size + 6.0f) : 20.0f;
      quad(scene.quads, {{area.origin.x, area.origin.y + 5.0f}, {3.0f, 16.0f}}, theme.text_accent, clip_rect);
      label(scene,
            node.label,
            D2D1::RectF(area.origin.x + 12.0f, area.origin.y + 1.0f, area.origin.x + area.extent.x - 8.0f, area.origin.y + 6.0f + text_height),
            theme.text_primary,
            TextStyle::body,
            clip_rect,
            attr_string(node, "font"));
      return;
    }
    case WidgetKind::button:
      quad(scene.quads, {{area.origin.x, area.origin.y}, {area.extent.x, 34.0f}}, theme.button_background, clip_rect);
      quad(scene.quads, {{area.origin.x + 2.0f, area.origin.y + 2.0f}, {area.extent.x - 4.0f, 30.0f}}, theme.button_highlight, clip_rect);
      stroke_rect(scene.quads, {{area.origin.x, area.origin.y}, {area.extent.x, 34.0f}}, 1.0f, theme.separator, clip_rect);
      label(scene,
            node.label,
            D2D1::RectF(area.origin.x + 8.0f, area.origin.y + 2.0f, area.origin.x + area.extent.x - 8.0f, area.origin.y + 32.0f),
            theme.text_primary,
            TextStyle::center,
            clip_rect);
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
      label(scene,
            node.label,
            D2D1::RectF(area.origin.x + 28.0f, area.origin.y, area.origin.x + area.extent.x, area.origin.y + 24.0f),
            theme.text_primary,
            TextStyle::body,
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
      quad(scene.quads, {{image_rect.origin.x, image_rect.origin.y}, {image_rect.extent.x, 2.0f}}, theme.text_accent, clip_rect);
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
        label(scene,
              node.label,
              D2D1::RectF(image_rect.origin.x, image_rect.origin.y + image_rect.extent.y + 4.0f, image_rect.origin.x + image_rect.extent.x,
                          image_rect.origin.y + image_rect.extent.y + 22.0f),
              theme.text_secondary,
              TextStyle::body,
              clip_rect);
      }
      return;
    }
    case WidgetKind::progress_bar: {
      const float value = std::clamp(attr_float(node, "value").value_or(0.0f), 0.0f, 1.0f);
      quad(scene.quads, {{area.origin.x, area.origin.y + 22.0f}, {area.extent.x, 12.0f}}, theme.progress_track, clip_rect);
      quad(scene.quads, {{area.origin.x, area.origin.y + 22.0f}, {area.extent.x * value, 12.0f}}, theme.progress_fill, clip_rect);
      stroke_rect(scene.quads, {{area.origin.x, area.origin.y + 22.0f}, {area.extent.x, 12.0f}}, 1.0f, theme.separator, clip_rect);
      label(scene,
            node.label,
            D2D1::RectF(area.origin.x, area.origin.y, area.origin.x + area.extent.x, area.origin.y + 18.0f),
            theme.text_secondary,
            TextStyle::body,
            clip_rect);
      return;
    }
    case WidgetKind::separator:
      quad(scene.quads, {{area.origin.x, area.origin.y + 2.0f}, {area.extent.x, 2.0f}}, theme.separator, clip_rect);
      return;
    case WidgetKind::custom_draw: {
      CustomDrawPrimitive primitive = CustomDrawPrimitive::fill_rect;
      const bool has_primitive = parse_custom_draw_primitive(attr_string(node, "primitive"), &primitive);
      const auto color = attr_color(node, "color_", theme.text_accent);
      if (has_primitive && primitive == CustomDrawPrimitive::fill_rect) {
        quad(scene.quads,
             {{area.origin.x + attr_float(node, "x").value_or(0.0f), area.origin.y + attr_float(node, "y").value_or(0.0f)},
              {attr_float(node, "width").value_or(0.0f), attr_float(node, "height").value_or(0.0f)}},
             color, clip_rect);
        return;
      }
      if (has_primitive && primitive == CustomDrawPrimitive::stroke_rect) {
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
      if (has_primitive && primitive == CustomDrawPrimitive::line) {
        const Vec2 from{area.origin.x + attr_float(node, "x1").value_or(0.0f), area.origin.y + attr_float(node, "y1").value_or(0.0f)};
        const Vec2 to{area.origin.x + attr_float(node, "x2").value_or(0.0f), area.origin.y + attr_float(node, "y2").value_or(0.0f)};
        line_quad(scene.quads, from, to, attr_float(node, "thickness").value_or(1.0f), color, clip_rect);
        return;
      }
      if (has_primitive && primitive == CustomDrawPrimitive::shader_rect) {
        const float shader_width = clamp_non_negative(attr_float(node, "width").value_or(0.0f));
        const float shader_height = clamp_non_negative(attr_float(node, "height").value_or(0.0f));
        Quad shader_quad{};
        shader_quad.points = {{
            {area.origin.x + attr_float(node, "x").value_or(0.0f), area.origin.y + attr_float(node, "y").value_or(0.0f)},
            {area.origin.x + attr_float(node, "x").value_or(0.0f), area.origin.y + attr_float(node, "y").value_or(0.0f) + shader_height},
            {area.origin.x + attr_float(node, "x").value_or(0.0f) + shader_width,
             area.origin.y + attr_float(node, "y").value_or(0.0f)},
            {area.origin.x + attr_float(node, "x").value_or(0.0f) + shader_width,
             area.origin.y + attr_float(node, "y").value_or(0.0f) + shader_height},
        }};
        shader_quad.color = attr_color(node, "tint_", {1.0f, 1.0f, 1.0f, 1.0f});
        shader_quad.texture_key = attr_string(node, "texture");
        const std::string_view image_key = attr_image_key(node);
        if (!image_key.empty()) {
          const auto resource_it = image_resources.find(image_key);
          if (resource_it != image_resources.end()) {
            shader_quad.texture_key = resource_it->second.texture_key;
            shader_quad.uv = {
                resource_it->second.uv.origin.x,
                resource_it->second.uv.origin.y,
                resource_it->second.uv.origin.x + resource_it->second.uv.extent.x,
                resource_it->second.uv.origin.y + resource_it->second.uv.extent.y,
            };
            shader_quad.color = multiply_color(shader_quad.color, resource_it->second.tint);
          }
        }
        shader_quad.shader_key = attr_shader_key(node);
        shader_quad.params = attr_shader_params(node);
        shader_quad.clip_rect = clip_rect;
        scene.quads.push_back(std::move(shader_quad));
        return;
      }
      if (has_primitive && primitive == CustomDrawPrimitive::shader_image) {
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
        quad(scene.quads, {{image_rect.origin.x, image_rect.origin.y}, {image_rect.extent.x, 2.0f}}, theme.text_accent, clip_rect);
        stroke_rect(scene.quads, image_rect, 1.0f, theme.separator, clip_rect);

        Quad shader_quad{};
        shader_quad.points = {{
            {image_rect.origin.x + 2.0f, image_rect.origin.y + 2.0f},
            {image_rect.origin.x + 2.0f, image_rect.origin.y + image_rect.extent.y - 2.0f},
            {image_rect.origin.x + image_rect.extent.x - 2.0f, image_rect.origin.y + 2.0f},
            {image_rect.origin.x + image_rect.extent.x - 2.0f, image_rect.origin.y + image_rect.extent.y - 2.0f},
        }};
        shader_quad.color = attr_color(node, "tint_", {1.0f, 1.0f, 1.0f, 1.0f});
        if (resource != nullptr) {
          shader_quad.texture_key = resource->texture_key;
          shader_quad.uv = {
              resource->uv.origin.x,
              resource->uv.origin.y,
              resource->uv.origin.x + resource->uv.extent.x,
              resource->uv.origin.y + resource->uv.extent.y,
          };
          shader_quad.color = multiply_color(shader_quad.color, resource->tint);
        } else {
          shader_quad.texture_key = attr_string(node, "texture");
        }
        shader_quad.shader_key = attr_shader_key(node);
        shader_quad.params = attr_shader_params(node);
        shader_quad.clip_rect = clip_rect;
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
      }
      return;
    }
  }
}

void build_scene(const FrameDocument& document,
                 const Dx12Theme& theme,
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
  out.push_back(a);
  out.push_back(b);
  out.push_back(c);
  out.push_back(c);
  out.push_back(b);
  out.push_back(d);
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

D3D12_RECT scissor_from_clip(const std::optional<Rect>& clip_rect, ExtentU viewport) {
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

Status validate_texture_binding(const Dx12TextureBinding& binding) {
  if (binding.heap == nullptr) {
    return Status::invalid_argument("Dx12Backend texture registration requires a descriptor heap.");
  }
  const D3D12_DESCRIPTOR_HEAP_DESC heap_desc = binding.heap->GetDesc();
  if (heap_desc.Type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
    return Status::invalid_argument("Dx12Backend texture registration requires a CBV/SRV/UAV descriptor heap.");
  }
  if ((heap_desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0) {
    return Status::invalid_argument("Dx12Backend texture registration requires a shader-visible descriptor heap.");
  }
  if (binding.cpu_descriptor.ptr == 0) {
    return Status::invalid_argument("Dx12Backend texture registration requires a valid CPU descriptor handle.");
  }
  if (binding.gpu_descriptor.ptr == 0) {
    return Status::invalid_argument("Dx12Backend texture registration requires a valid GPU descriptor handle.");
  }
  return Status::success();
}

Status validate_frame_binding(const Dx12FrameBinding& binding) {
  if (binding.command_list == nullptr) {
    return Status::invalid_argument("Dx12Backend frame binding requires a graphics command list.");
  }
  if (binding.render_target == nullptr) {
    return Status::invalid_argument("Dx12Backend frame binding requires a render target resource.");
  }
  if (binding.render_target_view.ptr == 0) {
    return Status::invalid_argument("Dx12Backend frame binding requires a valid render target view descriptor.");
  }
  if (binding.host_sets_descriptor_heaps && binding.shader_visible_srv_heap == nullptr) {
    return Status::invalid_argument("Dx12Backend frame binding requires a shader-visible SRV descriptor heap when the host manages descriptor heaps.");
  }
  return Status::success();
}

Status validate_document_textures(const WidgetNode& node,
                                  const StringMap<Dx12TextureBinding>& textures,
                                  const StringMap<ImageResourceDesc>& image_resources,
                                  const StringMap<ShaderResourceDesc>& shader_resources,
                                  const Dx12FrameBinding* frame_binding) {
  if (node.kind == WidgetKind::image) {
    const std::string_view image_key = attr_image_key(node);
    if (image_key.empty()) {
      return Status::invalid_argument("Dx12Backend image widgets require a non-empty image or texture key.");
    }
    const auto image_it = image_resources.find(image_key);
    const std::string_view texture_key = image_it != image_resources.end() ? std::string_view(image_it->second.texture_key) : image_key;
    const auto texture_it = textures.find(texture_key);
    if (texture_it == textures.end()) {
      return Status::invalid_argument("Dx12Backend image widget refers to an unregistered texture key: " + std::string(texture_key));
    }
    if (frame_binding != nullptr && frame_binding->host_sets_descriptor_heaps &&
        texture_it->second.heap != frame_binding->shader_visible_srv_heap) {
      return Status::invalid_argument("Dx12Backend texture binding heap does not match the host-bound shader-visible SRV heap for key: " + std::string(texture_key));
    }
  }
  if (node.kind == WidgetKind::custom_draw) {
    CustomDrawPrimitive primitive = CustomDrawPrimitive::fill_rect;
    if (parse_custom_draw_primitive(attr_string(node, "primitive"), &primitive) &&
        (primitive == CustomDrawPrimitive::shader_rect || primitive == CustomDrawPrimitive::shader_image)) {
      const std::string_view shader_key = attr_shader_key(node);
      if (shader_key.empty()) {
        return Status::invalid_argument("Dx12Backend shader widgets require a shader key.");
      }
      const auto shader_it = shader_resources.find(shader_key);
      if (shader_it == shader_resources.end()) {
        return Status::invalid_argument("Dx12Backend is missing the shader resource '" + std::string(shader_key) + "'.");
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
          return Status::invalid_argument("Dx12Backend shader widgets require a texture when the shader samples textures.");
        }
        const auto texture_it = textures.find(texture_key);
        if (texture_it == textures.end()) {
          return Status::invalid_argument("Dx12Backend shader widget refers to an unregistered texture key: " + std::string(texture_key));
        }
        if (frame_binding != nullptr && frame_binding->host_sets_descriptor_heaps &&
            texture_it->second.heap != frame_binding->shader_visible_srv_heap) {
          return Status::invalid_argument(
              "Dx12Backend shader texture binding heap does not match the host-bound shader-visible SRV heap for key: " + std::string(texture_key));
        }
      }
    }
  }
  for (const auto& child : node.children) {
    const Status child_status = validate_document_textures(child, textures, image_resources, shader_resources, frame_binding);
    if (!child_status) {
      return child_status;
    }
  }
  return Status::success();
}

Status validate_document_textures(const FrameDocument& document,
                                  const StringMap<Dx12TextureBinding>& textures,
                                  const StringMap<ImageResourceDesc>& image_resources,
                                  const StringMap<ShaderResourceDesc>& shader_resources,
                                  const Dx12FrameBinding* frame_binding) {
  for (const auto& root : document.roots) {
    const Status status = validate_document_textures(root, textures, image_resources, shader_resources, frame_binding);
    if (!status) {
      return status;
    }
  }
  return Status::success();
}

Status create_factory(IDXGIFactory4** factory, bool enable_debug_layer) {
  UINT flags = 0;
  if (enable_debug_layer) {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
      debug->EnableDebugLayer();
      flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
  const HRESULT hr = CreateDXGIFactory2(flags, IID_PPV_ARGS(factory));
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to create the DXGI factory: " + hex_hr(hr));
  }
  return Status::success();
}

Status create_owned_device(IDXGIFactory4* factory, ID3D12Device** device) {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  for (const auto level : levels) {
    HRESULT hr = D3D12CreateDevice(nullptr, level, IID_PPV_ARGS(device));
    if (SUCCEEDED(hr)) {
      return Status::success();
    }
    ComPtr<IDXGIAdapter> warp_adapter;
    if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter)))) {
      hr = D3D12CreateDevice(warp_adapter.Get(), level, IID_PPV_ARGS(device));
      if (SUCCEEDED(hr)) {
        return Status::success();
      }
    }
  }
  return Status::backend_error("Dx12Backend failed to create a D3D12 device.");
}

}  // namespace

class Dx12Backend::Impl {
 public:
  ComPtr<IDXGIFactory4> factory;
  ComPtr<ID3D12Device> owned_device;
  ID3D12Device* device{};
  ComPtr<ID3D12CommandQueue> owned_queue;
  ID3D12CommandQueue* queue{};
  ComPtr<IDXGISwapChain3> owned_swap_chain;
  IDXGISwapChain3* swap_chain{};
  std::array<OwnedFrameResources, kFrameCount> frames{};
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  UINT rtv_descriptor_size{};
  ComPtr<ID3D12GraphicsCommandList> owned_command_list;
  ComPtr<ID3D12Fence> owned_fence;
  HANDLE fence_event{};
  std::uint64_t next_fence_value{1};
  bool owned_frame_pending{false};
  UINT owned_pending_frame_index{};
  ComPtr<ID3D12RootSignature> root_signature;
  ComPtr<ID3D12PipelineState> solid_pso;
  ComPtr<ID3D12PipelineState> textured_pso;
  std::vector<std::uint8_t> default_vs_bytecode;
  ComPtr<ID3D12Resource> shader_constant_buffer;
  std::byte* shader_constant_buffer_mapped{};
  std::size_t shader_constant_capacity{};
  ComPtr<ID3D12DescriptorHeap> text_atlas_heap;
  ComPtr<ID3D12Resource> text_atlas_texture;
  ComPtr<ID3D12Resource> text_atlas_upload;
  std::byte* text_atlas_upload_mapped{};
  std::uint64_t text_atlas_upload_size{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT text_atlas_upload_footprint{};
  D3D12_RESOURCE_STATES text_atlas_state{D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE};
  D3D12_CPU_DESCRIPTOR_HANDLE text_atlas_cpu_descriptor{};
  D3D12_GPU_DESCRIPTOR_HANDLE text_atlas_gpu_descriptor{};
  std::uint32_t text_atlas_width{};
  std::uint32_t text_atlas_height{};
  std::uint64_t text_atlas_signature{};
  bool text_atlas_dirty{};
  std::uint32_t text_atlas_stable_frame_streak{};
  HDC text_bitmap_dc{};
  HBITMAP text_bitmap_handle{};
  HBITMAP text_bitmap_previous{};
  void* text_bitmap_bits{};
  std::uint32_t text_bitmap_width{};
  std::uint32_t text_bitmap_height{};
  StringMap<HFONT> gdi_fonts;
  ComPtr<ID3D11Device> interop_device;
  ComPtr<ID3D11DeviceContext> interop_context;
  ComPtr<ID3D11On12Device> on12_device;
  Dx12TextInteropApi text_interop_api{};
  ComPtr<ID2D1Factory3> d2d_factory;
  ComPtr<ID2D1Device2> d2d_device;
  ComPtr<ID2D1DeviceContext2> d2d_context;
  ComPtr<ID2D1SolidColorBrush> d2d_brush;
  ComPtr<IDWriteFactory> dwrite_factory;
  ComPtr<IDWriteTextFormat> title_format;
  ComPtr<IDWriteTextFormat> body_format;
  ComPtr<IDWriteTextFormat> center_format;
  ComPtr<ID3D12Resource> vertex_buffer;
  std::byte* vertex_buffer_mapped{};
  std::size_t vertex_capacity{};
  StringMap<FontResourceDesc> font_descriptors;
  StringMap<ComPtr<IDWriteTextFormat>> font_formats;
  StringMap<std::wstring> wide_text_cache;
  StringMap<ImageResourceDesc> image_resources;
  StringMap<ShaderResourceDesc> shader_descriptors;
  StringMap<Dx12CompiledShader> shaders;
  StringMap<Dx12TextureBinding> textures;
  BackendFrameStats stats{};
  BackendTelemetrySnapshot telemetry{};
  StringMap<ScopeAccumulator> scope_totals;
  std::uint64_t telemetry_refresh_count{};
  bool text_interop_available{false};
  std::uint32_t textless_frame_streak{};
  bool owned_command_list_executed{false};
  Scene scratch_scene;
  std::vector<TextAtlasPlacement> scratch_text_placements;
  std::vector<QuadVertex> scratch_vertices;
  std::vector<DrawBatch> scratch_batches;
};

void clear_owned_frame_state(Dx12Backend::Impl& impl) noexcept {
  impl.owned_frame_pending = false;
  impl.owned_command_list_executed = false;
  impl.owned_pending_frame_index = 0;
}

void clear_gdi_fonts(Dx12Backend::Impl& impl) noexcept {
  for (auto& [_, font] : impl.gdi_fonts) {
    if (font != nullptr) {
      DeleteObject(font);
    }
  }
  impl.gdi_fonts.clear();
}

void reset_text_bitmap_state(Dx12Backend::Impl& impl) noexcept {
  if (impl.text_bitmap_dc != nullptr && impl.text_bitmap_previous != nullptr) {
    SelectObject(impl.text_bitmap_dc, impl.text_bitmap_previous);
    impl.text_bitmap_previous = nullptr;
  }
  if (impl.text_bitmap_handle != nullptr) {
    DeleteObject(impl.text_bitmap_handle);
    impl.text_bitmap_handle = nullptr;
  }
  if (impl.text_bitmap_dc != nullptr) {
    DeleteDC(impl.text_bitmap_dc);
    impl.text_bitmap_dc = nullptr;
  }
  impl.text_bitmap_bits = nullptr;
  impl.text_bitmap_width = 0;
  impl.text_bitmap_height = 0;
}

void reset_text_atlas_upload_buffer(Dx12Backend::Impl& impl) noexcept;

void release_text_atlas_staging(Dx12Backend::Impl& impl) noexcept {
  reset_text_bitmap_state(impl);
  reset_text_atlas_upload_buffer(impl);
  clear_gdi_fonts(impl);
}

void reset_text_atlas_resources(Dx12Backend::Impl& impl) noexcept {
  if (impl.text_atlas_upload && impl.text_atlas_upload_mapped != nullptr) {
    impl.text_atlas_upload->Unmap(0, nullptr);
    impl.text_atlas_upload_mapped = nullptr;
  }
  impl.text_atlas_heap.Reset();
  impl.text_atlas_texture.Reset();
  impl.text_atlas_upload.Reset();
  impl.text_atlas_upload_size = 0;
  impl.text_atlas_upload_footprint = {};
  impl.text_atlas_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  impl.text_atlas_cpu_descriptor = {};
  impl.text_atlas_gpu_descriptor = {};
  impl.text_atlas_width = 0;
  impl.text_atlas_height = 0;
}

void reset_text_atlas_upload_buffer(Dx12Backend::Impl& impl) noexcept {
  if (impl.text_atlas_upload && impl.text_atlas_upload_mapped != nullptr) {
    impl.text_atlas_upload->Unmap(0, nullptr);
    impl.text_atlas_upload_mapped = nullptr;
  }
  impl.text_atlas_upload.Reset();
  impl.text_atlas_upload_size = 0;
  impl.text_atlas_upload_footprint = {};
}

int to_gdi_weight(FontWeight weight) noexcept {
  switch (weight) {
    case FontWeight::regular:
      return FW_NORMAL;
    case FontWeight::medium:
      return 500;
    case FontWeight::semibold:
      return FW_SEMIBOLD;
    case FontWeight::bold:
      return FW_BOLD;
  }
  return FW_NORMAL;
}

std::uint32_t resolve_text_dpi(HDC dc) noexcept {
  constexpr std::uint32_t kFallbackDpi = 96;
  if (dc == nullptr) {
    return kFallbackDpi;
  }
  const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
  return dpi > 0 ? static_cast<std::uint32_t>(dpi) : kFallbackDpi;
}

std::uint32_t rgba_row_pitch(std::uint32_t width) noexcept {
  return (width * 4u + 255u) & ~255u;
}

void reset_text_atlas_state(Dx12Backend::Impl& impl) noexcept {
  impl.textures.erase(std::string(kInternalTextAtlasKey));
  release_text_atlas_staging(impl);
  reset_text_atlas_resources(impl);
  impl.scratch_text_placements.clear();
  impl.text_atlas_signature = 0;
  impl.text_atlas_dirty = false;
  impl.text_atlas_stable_frame_streak = 0;
}

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

HFONT resolve_gdi_font(Dx12Backend::Impl& impl,
                       const Dx12BackendConfig& config,
                       const TextLabel& label) {
  std::string key;
  std::wstring family;
  float size = config.theme.body_text_size;
  int weight = FW_NORMAL;
  bool italic = false;

  if (!label.font_key.empty()) {
    key = std::string(label.font_key);
    const auto descriptor_it = impl.font_descriptors.find(label.font_key);
    if (descriptor_it != impl.font_descriptors.end()) {
      const FontResourceDesc& descriptor = descriptor_it->second;
      family = wide(descriptor.family.empty() ? std::string_view("Segoe UI") : std::string_view(descriptor.family));
      size = descriptor.size > 0.0f ? descriptor.size : config.theme.body_text_size;
      weight = to_gdi_weight(descriptor.weight);
      italic = descriptor.style == FontStyle::italic;
    }
  } else {
    switch (label.style) {
      case TextStyle::title:
        key = "__title__";
        family = L"Segoe UI";
        size = config.theme.title_text_size;
        weight = FW_SEMIBOLD;
        break;
      case TextStyle::center:
        key = "__center__";
        family = L"Segoe UI";
        size = config.theme.body_text_size;
        weight = FW_SEMIBOLD;
        break;
      case TextStyle::body:
        key = "__body__";
        family = L"Segoe UI";
        size = config.theme.body_text_size;
        weight = FW_NORMAL;
        break;
    }
  }

  if (family.empty()) {
    family = L"Segoe UI";
  }

  const auto cached = impl.gdi_fonts.find(key);
  if (cached != impl.gdi_fonts.end()) {
    return cached->second;
  }

  if (impl.text_bitmap_dc == nullptr) {
    return nullptr;
  }

  const int height = -MulDiv(static_cast<int>(std::lround(size)), static_cast<int>(resolve_text_dpi(impl.text_bitmap_dc)), 72);
  HFONT font = CreateFontW(
      height,
      0,
      0,
      0,
      weight,
      italic ? TRUE : FALSE,
      FALSE,
      FALSE,
      DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS,
      CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE,
      family.c_str());
  if (font != nullptr) {
    impl.gdi_fonts.emplace(std::move(key), font);
  }
  return font;
}

Status ensure_text_bitmap_surface(Dx12Backend::Impl& impl, std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    reset_text_bitmap_state(impl);
    return Status::success();
  }
  if (impl.text_bitmap_dc != nullptr && impl.text_bitmap_width == width && impl.text_bitmap_height == height && impl.text_bitmap_bits != nullptr) {
    return Status::success();
  }

  reset_text_bitmap_state(impl);

  impl.text_bitmap_dc = CreateCompatibleDC(nullptr);
  if (impl.text_bitmap_dc == nullptr) {
    return Status::backend_error("Dx12Backend failed to create the software text bitmap DC.");
  }

  BITMAPINFO bitmap_info{};
  bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap_info.bmiHeader.biWidth = static_cast<LONG>(width);
  bitmap_info.bmiHeader.biHeight = -static_cast<LONG>(height);
  bitmap_info.bmiHeader.biPlanes = 1;
  bitmap_info.bmiHeader.biBitCount = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;

  impl.text_bitmap_handle = CreateDIBSection(impl.text_bitmap_dc, &bitmap_info, DIB_RGB_COLORS, &impl.text_bitmap_bits, nullptr, 0);
  if (impl.text_bitmap_handle == nullptr || impl.text_bitmap_bits == nullptr) {
    reset_text_bitmap_state(impl);
    return Status::backend_error("Dx12Backend failed to create the software text bitmap surface.");
  }

  impl.text_bitmap_previous = static_cast<HBITMAP>(SelectObject(impl.text_bitmap_dc, impl.text_bitmap_handle));
  impl.text_bitmap_width = width;
  impl.text_bitmap_height = height;
  SetBkMode(impl.text_bitmap_dc, TRANSPARENT);
  SetTextColor(impl.text_bitmap_dc, RGB(255, 255, 255));
  SetTextAlign(impl.text_bitmap_dc, TA_LEFT | TA_TOP);
  return Status::success();
}

UINT atlas_draw_flags(const TextLabel& label) noexcept {
  UINT flags = DT_NOPREFIX | DT_SINGLELINE;
  switch (label.style) {
    case TextStyle::title:
      flags |= DT_LEFT | DT_VCENTER;
      break;
    case TextStyle::center:
      flags |= DT_CENTER | DT_VCENTER;
      break;
    case TextStyle::body:
      flags |= DT_LEFT | DT_TOP;
      break;
  }
  return flags;
}

TextAtlasPlacement measure_text_label(Dx12Backend::Impl& impl, const Dx12BackendConfig& config, const TextLabel& label) {
  const auto available_width = static_cast<std::uint32_t>((std::max)(1.0f, std::ceil(label.rect.right - label.rect.left)));
  const auto available_height = static_cast<std::uint32_t>((std::max)(1.0f, std::ceil(label.rect.bottom - label.rect.top)));

  std::uint32_t measured_width = 1;
  std::uint32_t measured_height = 1;
  if (impl.text_bitmap_dc != nullptr && label.text != nullptr && !label.text->empty()) {
    HFONT font = resolve_gdi_font(impl, config, label);
    HGDIOBJ previous_font = nullptr;
    if (font != nullptr) {
      previous_font = SelectObject(impl.text_bitmap_dc, font);
    }

    RECT measure_rect{
        0,
        0,
        static_cast<LONG>(available_width),
        static_cast<LONG>(available_height),
    };
    const UINT measure_flags = atlas_draw_flags(label) | DT_CALCRECT;
    if (DrawTextW(impl.text_bitmap_dc, label.text->c_str(), static_cast<int>(label.text->size()), &measure_rect, measure_flags) > 0) {
      measured_width = static_cast<std::uint32_t>((std::max)(1L, measure_rect.right - measure_rect.left));
      measured_height = static_cast<std::uint32_t>((std::max)(1L, measure_rect.bottom - measure_rect.top));
    }

    if (previous_font != nullptr) {
      SelectObject(impl.text_bitmap_dc, previous_font);
    }
  }

  const std::uint32_t packed_width = (std::max)(1u, (std::min)(available_width, measured_width));
  const std::uint32_t packed_height = (std::max)(1u, (std::min)(available_height, measured_height));

  float output_left = label.rect.left;
  float output_top = label.rect.top;
  const float available_width_f = label.rect.right - label.rect.left;
  const float available_height_f = label.rect.bottom - label.rect.top;
  if (label.style == TextStyle::center) {
    output_left += (std::max)(0.0f, (available_width_f - static_cast<float>(packed_width)) * 0.5f);
    output_top += (std::max)(0.0f, (available_height_f - static_cast<float>(packed_height)) * 0.5f);
  } else if (label.style == TextStyle::title) {
    output_top += (std::max)(0.0f, (available_height_f - static_cast<float>(packed_height)) * 0.5f);
  }

  return {
      .label = &label,
      .atlas_x = 0,
      .atlas_y = 0,
      .width = packed_width,
      .height = packed_height,
      .output_rect = {
          .origin = {output_left, output_top},
          .extent = {static_cast<float>(packed_width), static_cast<float>(packed_height)},
      },
  };
}

bool pack_text_labels(Dx12Backend::Impl& impl,
                      const Dx12BackendConfig& config,
                      const std::vector<TextLabel>& labels,
                      std::uint32_t max_dimension,
                      std::uint32_t padding,
                      std::vector<TextAtlasPlacement>& placements,
                      std::uint32_t& atlas_width,
                      std::uint32_t& atlas_height) {
  placements.clear();
  atlas_width = 0;
  atlas_height = 0;
  if (labels.empty()) {
    return true;
  }

  const std::uint32_t clamped_padding = (std::max)(padding, 1u);
  const std::uint32_t max_size = (std::max)(max_dimension, 64u);
  std::uint32_t cursor_x = clamped_padding;
  std::uint32_t cursor_y = clamped_padding;
  std::uint32_t row_height = 0;
  std::uint32_t used_width = 0;

  placements.reserve(labels.size());
  for (const auto& label : labels) {
    TextAtlasPlacement placement = measure_text_label(impl, config, label);
    if (placement.width + clamped_padding * 2 > max_size || placement.height + clamped_padding * 2 > max_size) {
      return false;
    }
    if (cursor_x + placement.width + clamped_padding > max_size) {
      cursor_x = clamped_padding;
      cursor_y += row_height + clamped_padding;
      row_height = 0;
    }
    if (cursor_y + placement.height + clamped_padding > max_size) {
      return false;
    }
    placement.atlas_x = cursor_x;
    placement.atlas_y = cursor_y;
    placements.push_back(placement);
    cursor_x += placement.width + clamped_padding;
    row_height = (std::max)(row_height, placement.height);
    used_width = (std::max)(used_width, cursor_x);
  }

  atlas_width = next_power_of_two((std::max)(used_width + clamped_padding, 64u));
  atlas_height = next_power_of_two((std::max)(cursor_y + row_height + clamped_padding, 64u));
  atlas_width = (std::min)(atlas_width, max_size);
  atlas_height = (std::min)(atlas_height, max_size);
  return true;
}

void normalize_text_bitmap_alpha(Dx12Backend::Impl& impl, std::uint32_t width, std::uint32_t height) {
  auto* pixels = static_cast<std::uint8_t*>(impl.text_bitmap_bits);
  if (pixels == nullptr) {
    return;
  }
  const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  for (std::size_t index = 0; index < pixel_count; ++index) {
    std::uint8_t* pixel = pixels + index * 4;
    const std::uint8_t alpha = (std::max)((std::max)(pixel[0], pixel[1]), pixel[2]);
    if (alpha == 0) {
      pixel[0] = 0;
      pixel[1] = 0;
      pixel[2] = 0;
      pixel[3] = 0;
      continue;
    }
    pixel[0] = 0xFF;
    pixel[1] = 0xFF;
    pixel[2] = 0xFF;
    pixel[3] = alpha;
  }
}

Status ensure_text_atlas_texture(Dx12Backend::Impl& impl, std::uint32_t width, std::uint32_t height) {
  if (impl.device == nullptr || width == 0 || height == 0) {
    return Status::success();
  }

  if (!impl.text_atlas_heap) {
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    const HRESULT heap_hr = impl.device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&impl.text_atlas_heap));
    if (FAILED(heap_hr)) {
      return Status::backend_error("Dx12Backend failed to create the internal text atlas descriptor heap: " + hex_hr(heap_hr));
    }
    impl.text_atlas_cpu_descriptor = impl.text_atlas_heap->GetCPUDescriptorHandleForHeapStart();
    impl.text_atlas_gpu_descriptor = impl.text_atlas_heap->GetGPUDescriptorHandleForHeapStart();
  }

  if (!impl.text_atlas_texture || impl.text_atlas_width != width || impl.text_atlas_height != height) {
    impl.text_atlas_texture.Reset();
    impl.text_atlas_upload.Reset();
    impl.text_atlas_upload_size = 0;

    D3D12_HEAP_PROPERTIES heap_props{};
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC resource_desc{};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource_desc.Width = width;
    resource_desc.Height = height;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = kRenderTargetFormat;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    const HRESULT texture_hr = impl.device->CreateCommittedResource(
        &heap_props,
        D3D12_HEAP_FLAG_NONE,
        &resource_desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&impl.text_atlas_texture));
    if (FAILED(texture_hr)) {
      return Status::backend_error("Dx12Backend failed to create the internal text atlas texture: " + hex_hr(texture_hr));
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = kRenderTargetFormat;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Texture2D.MipLevels = 1;
    impl.device->CreateShaderResourceView(impl.text_atlas_texture.Get(), &srv_desc, impl.text_atlas_cpu_descriptor);

    impl.text_atlas_width = width;
    impl.text_atlas_height = height;
    impl.text_atlas_state = D3D12_RESOURCE_STATE_COPY_DEST;
  }

  return Status::success();
}

Status ensure_text_atlas_upload_buffer(Dx12Backend::Impl& impl) {
  if (impl.device == nullptr || !impl.text_atlas_texture) {
    return Status::success();
  }

  const auto texture_desc = impl.text_atlas_texture->GetDesc();
  UINT row_count = 0;
  UINT64 row_size = 0;
  std::uint64_t upload_size = 0;
  impl.device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &impl.text_atlas_upload_footprint, &row_count, &row_size, &upload_size);
  if (impl.text_atlas_upload && upload_size <= impl.text_atlas_upload_size) {
    return Status::success();
  }

  reset_text_atlas_upload_buffer(impl);
  impl.device->GetCopyableFootprints(&texture_desc, 0, 1, 0, &impl.text_atlas_upload_footprint, &row_count, &row_size, &upload_size);

  D3D12_HEAP_PROPERTIES heap_props{};
  heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC buffer_desc{};
  buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer_desc.Width = upload_size;
  buffer_desc.Height = 1;
  buffer_desc.DepthOrArraySize = 1;
  buffer_desc.MipLevels = 1;
  buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  buffer_desc.SampleDesc.Count = 1;

  const HRESULT hr = impl.device->CreateCommittedResource(
      &heap_props,
      D3D12_HEAP_FLAG_NONE,
      &buffer_desc,
      D3D12_RESOURCE_STATE_GENERIC_READ,
      nullptr,
      IID_PPV_ARGS(&impl.text_atlas_upload));
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to create the text atlas upload buffer: " + hex_hr(hr));
  }
  void* mapped = nullptr;
  const HRESULT map_hr = impl.text_atlas_upload->Map(0, nullptr, &mapped);
  if (FAILED(map_hr)) {
    impl.text_atlas_upload.Reset();
    return Status::backend_error("Dx12Backend failed to map the text atlas upload buffer: " + hex_hr(map_hr));
  }
  impl.text_atlas_upload_mapped = static_cast<std::byte*>(mapped);
  impl.text_atlas_upload_size = upload_size;
  return Status::success();
}

Status upload_text_atlas(Dx12Backend::Impl& impl, ID3D12GraphicsCommandList* command_list) {
  if (command_list == nullptr || impl.text_atlas_width == 0 || impl.text_atlas_height == 0 || !impl.text_atlas_dirty) {
    return Status::success();
  }
  if (impl.text_bitmap_bits == nullptr) {
    return Status::not_ready("Dx12Backend text atlas upload requires a populated software text bitmap.");
  }

  Status status = ensure_text_atlas_texture(impl, impl.text_bitmap_width, impl.text_bitmap_height);
  if (!status) {
    return status;
  }
  status = ensure_text_atlas_upload_buffer(impl);
  if (!status) {
    return status;
  }
  if (!impl.text_atlas_texture || !impl.text_atlas_upload) {
    return Status::success();
  }
  if (impl.text_atlas_upload_mapped == nullptr) {
    return Status::not_ready("Dx12Backend text atlas upload buffer is not mapped.");
  }

  const auto* source = static_cast<const std::byte*>(impl.text_bitmap_bits);
  const std::size_t source_row_pitch = static_cast<std::size_t>(impl.text_bitmap_width) * 4u;
  for (UINT row = 0; row < impl.text_atlas_upload_footprint.Footprint.Height; ++row) {
    std::memcpy(impl.text_atlas_upload_mapped + impl.text_atlas_upload_footprint.Offset +
                    static_cast<std::size_t>(row) * impl.text_atlas_upload_footprint.Footprint.RowPitch,
                source + static_cast<std::size_t>(row) * source_row_pitch,
                source_row_pitch);
  }

  if (impl.text_atlas_state != D3D12_RESOURCE_STATE_COPY_DEST) {
    D3D12_RESOURCE_BARRIER to_copy{};
    to_copy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    to_copy.Transition.pResource = impl.text_atlas_texture.Get();
    to_copy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    to_copy.Transition.StateBefore = impl.text_atlas_state;
    to_copy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    command_list->ResourceBarrier(1, &to_copy);
  }

  D3D12_TEXTURE_COPY_LOCATION destination{};
  destination.pResource = impl.text_atlas_texture.Get();
  destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  destination.SubresourceIndex = 0;
  D3D12_TEXTURE_COPY_LOCATION source_location{};
  source_location.pResource = impl.text_atlas_upload.Get();
  source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  source_location.PlacedFootprint = impl.text_atlas_upload_footprint;
  command_list->CopyTextureRegion(&destination, 0, 0, 0, &source_location, nullptr);

  D3D12_RESOURCE_BARRIER to_sample{};
  to_sample.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  to_sample.Transition.pResource = impl.text_atlas_texture.Get();
  to_sample.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  to_sample.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  to_sample.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, &to_sample);
  impl.text_atlas_state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  impl.text_atlas_dirty = false;
  impl.text_atlas_stable_frame_streak = 0;
  return Status::success();
}

Status append_text_atlas_quads(Dx12Backend::Impl& impl, const Dx12BackendConfig& config, std::vector<Quad>& quads) {
  if (impl.scratch_scene.labels.empty()) {
    impl.text_atlas_width = 0;
    impl.text_atlas_height = 0;
    impl.scratch_text_placements.clear();
    return Status::success();
  }

  const std::uint32_t max_dimension = static_cast<std::uint32_t>((std::max<std::size_t>)(
      64u,
      (std::min)(config.text_atlas.max_dimension, static_cast<std::uint32_t>(config.resource_budgets.max_text_atlas_dimension))));
  const std::uint32_t padding = static_cast<std::uint32_t>((std::max<std::size_t>)(
      1u,
      (std::max)(config.text_atlas.padding, static_cast<std::uint32_t>(config.resource_budgets.text_atlas_padding))));

  Status status = ensure_text_bitmap_surface(impl, 1, 1);
  if (!status) {
    return status;
  }

  std::uint32_t atlas_width = 0;
  std::uint32_t atlas_height = 0;
  if (!pack_text_labels(impl, config, impl.scratch_scene.labels, max_dimension, padding, impl.scratch_text_placements, atlas_width, atlas_height)) {
    return Status::backend_error("Dx12Backend text atlas packing exceeded the configured atlas budget.");
  }

  const std::uint64_t atlas_signature = text_atlas_signature(impl.scratch_text_placements);
  const bool atlas_content_changed =
      impl.text_atlas_width != atlas_width || impl.text_atlas_height != atlas_height || impl.text_atlas_signature != atlas_signature;
  if (atlas_content_changed) {
    status = ensure_text_bitmap_surface(impl, atlas_width, atlas_height);
    if (!status) {
      return status;
    }
    if (impl.text_bitmap_bits == nullptr || impl.text_bitmap_dc == nullptr) {
      return Status::backend_error("Dx12Backend software text bitmap surface is unavailable.");
    }

    std::memset(impl.text_bitmap_bits, 0, estimate_bitmap_bytes(atlas_width, atlas_height));
    SetBkMode(impl.text_bitmap_dc, TRANSPARENT);
    SetTextColor(impl.text_bitmap_dc, RGB(255, 255, 255));

    for (const auto& placement : impl.scratch_text_placements) {
      const TextLabel& label = *placement.label;
      HFONT font = resolve_gdi_font(impl, config, label);
      HGDIOBJ previous_font = nullptr;
      if (font != nullptr) {
        previous_font = SelectObject(impl.text_bitmap_dc, font);
      }

      RECT draw_rect{
          static_cast<LONG>(placement.atlas_x),
          static_cast<LONG>(placement.atlas_y),
          static_cast<LONG>(placement.atlas_x + placement.width),
          static_cast<LONG>(placement.atlas_y + placement.height),
      };
      const UINT draw_flags = atlas_draw_flags(label);
      DrawTextW(impl.text_bitmap_dc, label.text->c_str(), static_cast<int>(label.text->size()), &draw_rect, draw_flags);
      if (previous_font != nullptr) {
        SelectObject(impl.text_bitmap_dc, previous_font);
      }
    }

    normalize_text_bitmap_alpha(impl, atlas_width, atlas_height);
    impl.text_atlas_signature = atlas_signature;
    impl.text_atlas_dirty = true;
    impl.text_atlas_stable_frame_streak = 0;
  } else {
    impl.text_atlas_dirty = false;
    impl.text_atlas_stable_frame_streak += 1;
  }

  impl.text_atlas_width = atlas_width;
  impl.text_atlas_height = atlas_height;

  quads.reserve(quads.size() + impl.scratch_text_placements.size());
  for (const auto& placement : impl.scratch_text_placements) {
    const TextLabel& label = *placement.label;
    const float left = placement.output_rect.origin.x;
    const float top = placement.output_rect.origin.y;
    const float right = placement.output_rect.origin.x + placement.output_rect.extent.x;
    const float bottom = placement.output_rect.origin.y + placement.output_rect.extent.y;
    quads.push_back({
        .points = {{
            {left, top},
            {left, bottom},
            {right, top},
            {right, bottom},
        }},
        .color = label.color,
        .uv = {
            static_cast<float>(placement.atlas_x) / static_cast<float>(atlas_width),
            static_cast<float>(placement.atlas_y) / static_cast<float>(atlas_height),
            static_cast<float>(placement.atlas_x + placement.width) / static_cast<float>(atlas_width),
            static_cast<float>(placement.atlas_y + placement.height) / static_cast<float>(atlas_height),
        },
        .texture_key = kInternalTextAtlasKey,
        .clip_rect = label.clip_rect,
    });
  }

  return Status::success();
}

Status ensure_vertex_buffer(Dx12Backend::Impl& impl, std::size_t vertex_count) {
  if (vertex_count == 0) {
    return Status::success();
  }
  constexpr std::size_t kMaxVertexCount = static_cast<std::size_t>((std::numeric_limits<UINT64>::max)() / sizeof(QuadVertex));
  if (vertex_count > kMaxVertexCount) {
    return Status::invalid_argument("Dx12Backend vertex submission exceeds the upload buffer size limit.");
  }
  if (impl.vertex_buffer && vertex_count <= impl.vertex_capacity) {
    return Status::success();
  }
  if (impl.vertex_buffer && impl.vertex_buffer_mapped != nullptr) {
    impl.vertex_buffer->Unmap(0, nullptr);
    impl.vertex_buffer_mapped = nullptr;
  }
  impl.vertex_buffer.Reset();
  const std::size_t new_capacity = std::max(vertex_count, std::max<std::size_t>(256, impl.vertex_capacity * 2));
  D3D12_HEAP_PROPERTIES heap_props{};
  heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC resource_desc{};
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  resource_desc.Width = static_cast<UINT64>(new_capacity * sizeof(QuadVertex));
  resource_desc.Height = 1;
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  resource_desc.SampleDesc.Count = 1;
  const HRESULT hr = impl.device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &resource_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr, IID_PPV_ARGS(&impl.vertex_buffer));
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to create the upload vertex buffer: " + hex_hr(hr));
  }
  void* mapped = nullptr;
  const HRESULT map_hr = impl.vertex_buffer->Map(0, nullptr, &mapped);
  if (FAILED(map_hr)) {
    impl.vertex_buffer.Reset();
    return Status::backend_error("Dx12Backend failed to map the upload vertex buffer: " + hex_hr(map_hr));
  }
  impl.vertex_buffer_mapped = static_cast<std::byte*>(mapped);
  impl.vertex_capacity = new_capacity;
  return Status::success();
}

std::size_t align_constant_buffer_size(std::size_t size) {
  constexpr std::size_t kAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
  return (size + (kAlignment - 1)) & ~(kAlignment - 1);
}

Status ensure_shader_constant_buffer(Dx12Backend::Impl& impl, std::size_t batch_count) {
  const std::size_t required_capacity = std::max<std::size_t>(1, batch_count);
  const std::size_t aligned_constant_size = align_constant_buffer_size(sizeof(ShaderConstants));
  if (required_capacity > static_cast<std::size_t>((std::numeric_limits<UINT64>::max)() / aligned_constant_size)) {
    return Status::invalid_argument("Dx12Backend shader batch count exceeds the constant buffer size limit.");
  }
  if (impl.shader_constant_buffer && required_capacity <= impl.shader_constant_capacity) {
    return Status::success();
  }
  if (impl.shader_constant_buffer && impl.shader_constant_buffer_mapped != nullptr) {
    impl.shader_constant_buffer->Unmap(0, nullptr);
    impl.shader_constant_buffer_mapped = nullptr;
  }
  impl.shader_constant_buffer.Reset();

  D3D12_HEAP_PROPERTIES heap_props{};
  heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC resource_desc{};
  resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  resource_desc.Width = aligned_constant_size * required_capacity;
  resource_desc.Height = 1;
  resource_desc.DepthOrArraySize = 1;
  resource_desc.MipLevels = 1;
  resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  resource_desc.SampleDesc.Count = 1;
  const HRESULT hr = impl.device->CreateCommittedResource(&heap_props,
                                                          D3D12_HEAP_FLAG_NONE,
                                                          &resource_desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ,
                                                          nullptr,
                                                          IID_PPV_ARGS(&impl.shader_constant_buffer));
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to create the shader constant buffer: " + hex_hr(hr));
  }
  void* mapped = nullptr;
  const HRESULT map_hr = impl.shader_constant_buffer->Map(0, nullptr, &mapped);
  if (FAILED(map_hr)) {
    impl.shader_constant_buffer.Reset();
    return Status::backend_error("Dx12Backend failed to map the shader constant buffer: " + hex_hr(map_hr));
  }
  impl.shader_constant_buffer_mapped = static_cast<std::byte*>(mapped);
  impl.shader_constant_capacity = required_capacity;
  return Status::success();
}

Status wait_for_fence(Dx12Backend::Impl& impl, std::uint64_t fence_value) {
  if (!impl.owned_fence || fence_value == 0) {
    return Status::success();
  }
  if (impl.owned_fence->GetCompletedValue() >= fence_value) {
    return Status::success();
  }
  const HRESULT hr = impl.owned_fence->SetEventOnCompletion(fence_value, impl.fence_event);
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to wait on the GPU fence: " + hex_hr(hr));
  }
  WaitForSingleObject(impl.fence_event, INFINITE);
  return Status::success();
}

Status wait_for_gpu_idle(Dx12Backend::Impl& impl) {
  if (!impl.queue || !impl.owned_fence) {
    return Status::success();
  }
  const std::uint64_t fence_value = impl.next_fence_value++;
  const HRESULT hr = impl.queue->Signal(impl.owned_fence.Get(), fence_value);
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to signal the GPU fence: " + hex_hr(hr));
  }
  return wait_for_fence(impl, fence_value);
}

void unload_text_interop_api(Dx12TextInteropApi& api) noexcept {
  api.create_d3d11on12_device = nullptr;
  api.create_d2d1_factory = nullptr;
  api.create_dwrite_factory = nullptr;
  if (api.dwrite_module != nullptr) {
    FreeLibrary(api.dwrite_module);
    api.dwrite_module = nullptr;
  }
  if (api.d2d1_module != nullptr) {
    FreeLibrary(api.d2d1_module);
    api.d2d1_module = nullptr;
  }
  if (api.d3d11_module != nullptr) {
    FreeLibrary(api.d3d11_module);
    api.d3d11_module = nullptr;
  }
}

Status ensure_text_interop_api(Dx12TextInteropApi& api) {
  auto load_module = [](HMODULE* module, const wchar_t* name) -> bool {
    if (*module == nullptr) {
      *module = LoadLibraryW(name);
    }
    return *module != nullptr;
  };
  auto resolve = [](HMODULE module, const char* name) -> FARPROC {
    return module != nullptr ? GetProcAddress(module, name) : nullptr;
  };

  if (!load_module(&api.d3d11_module, L"d3d11.dll") ||
      !load_module(&api.d2d1_module, L"d2d1.dll") ||
      !load_module(&api.dwrite_module, L"dwrite.dll")) {
    unload_text_interop_api(api);
    return Status::not_ready("Dx12Backend interop text dependencies are unavailable.");
  }

  api.create_d3d11on12_device = reinterpret_cast<D3D11On12CreateDeviceFn>(resolve(api.d3d11_module, "D3D11On12CreateDevice"));
  api.create_d2d1_factory = reinterpret_cast<D2D1CreateFactoryFn>(resolve(api.d2d1_module, "D2D1CreateFactory"));
  api.create_dwrite_factory = reinterpret_cast<DWriteCreateFactoryFn>(resolve(api.dwrite_module, "DWriteCreateFactory"));
  if (api.create_d3d11on12_device == nullptr || api.create_d2d1_factory == nullptr || api.create_dwrite_factory == nullptr) {
    unload_text_interop_api(api);
    return Status::not_ready("Dx12Backend interop text entrypoints could not be resolved.");
  }

  return Status::success();
}

void reset_text_interop_state(Dx12Backend::Impl& impl) noexcept {
  if (impl.d2d_context) {
    impl.d2d_context->SetTarget(nullptr);
  }
  if (impl.interop_context) {
    impl.interop_context->ClearState();
    impl.interop_context->Flush();
  }
  for (auto& frame : impl.frames) {
    frame.wrapped_back_buffer.Reset();
    frame.text_bitmap.Reset();
  }
  impl.font_formats.clear();
  impl.title_format.Reset();
  impl.body_format.Reset();
  impl.center_format.Reset();
  impl.dwrite_factory.Reset();
  impl.d2d_brush.Reset();
  impl.d2d_context.Reset();
  impl.d2d_device.Reset();
  impl.d2d_factory.Reset();
  impl.on12_device.Reset();
  impl.interop_context.Reset();
  impl.interop_device.Reset();
  unload_text_interop_api(impl.text_interop_api);
  impl.text_interop_available = false;
}

void clear_text_targets(Dx12Backend::Impl& impl) noexcept {
  if (impl.d2d_context) {
    impl.d2d_context->SetTarget(nullptr);
  }
  if (impl.interop_context) {
    impl.interop_context->ClearState();
    impl.interop_context->Flush();
  }
  for (auto& frame : impl.frames) {
    frame.wrapped_back_buffer.Reset();
    frame.text_bitmap.Reset();
  }
}

Status Dx12Backend::create_text_interop() {
  reset_text_interop_state(*impl_);
  if (config_.text_renderer != Dx12TextRendererMode::interop) {
    impl_->text_interop_available = false;
    return Status::success();
  }
  if (!owns_device_objects_ || impl_->device == nullptr || impl_->queue == nullptr) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  // Keep the DX12 debug layer decision independent from the D3D11On12 text bridge.
  // Enabling both debug stacks at once drives up the DX12 sample working set sharply.
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_FEATURE_LEVEL feature_levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  IUnknown* queues[] = {impl_->queue};
  D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_11_0;
  const Status api_status = ensure_text_interop_api(impl_->text_interop_api);
  if (!api_status) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  HRESULT hr = impl_->text_interop_api.create_d3d11on12_device(
      impl_->device,
      flags,
      feature_levels,
      static_cast<UINT>(std::size(feature_levels)),
      queues,
      1,
      0,
      &impl_->interop_device,
      &impl_->interop_context,
      &created_level);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  hr = impl_->interop_device.As(&impl_->on12_device);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
  if (config_.enable_debug_layer) {
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
  }
#endif
  hr = impl_->text_interop_api.create_d2d1_factory(
      D2D1_FACTORY_TYPE_SINGLE_THREADED,
      __uuidof(ID2D1Factory3),
      &options,
      reinterpret_cast<void**>(impl_->d2d_factory.GetAddressOf()));
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  ComPtr<IDXGIDevice> dxgi_device;
  hr = impl_->interop_device.As(&dxgi_device);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  hr = impl_->d2d_factory->CreateDevice(dxgi_device.Get(), &impl_->d2d_device);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }
  hr = impl_->d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &impl_->d2d_context);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }
  impl_->d2d_context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  impl_->d2d_context->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

  hr = impl_->text_interop_api.create_dwrite_factory(
      DWRITE_FACTORY_TYPE_SHARED,
      __uuidof(IDWriteFactory),
      reinterpret_cast<IUnknown**>(impl_->dwrite_factory.GetAddressOf()));
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  hr = impl_->d2d_context->CreateSolidColorBrush(to_d2d_color(config_.theme.text_primary), &impl_->d2d_brush);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }

  hr = impl_->dwrite_factory->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, config_.theme.title_text_size, L"en-us",
      &impl_->title_format);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }
  impl_->title_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  impl_->title_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  hr = impl_->dwrite_factory->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, config_.theme.body_text_size, L"en-us",
      &impl_->body_format);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }
  impl_->body_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  impl_->body_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  impl_->body_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

  hr = impl_->dwrite_factory->CreateTextFormat(
      L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, config_.theme.body_text_size, L"en-us",
      &impl_->center_format);
  if (FAILED(hr)) {
    impl_->text_interop_available = false;
    return Status::success();
  }
  impl_->center_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  impl_->center_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  impl_->center_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  const Status font_status = realize_registered_fonts();
  if (!font_status) {
    impl_->text_interop_available = false;
    return font_status;
  }

  impl_->text_interop_available = true;
  return Status::success();
}

Status Dx12Backend::realize_registered_fonts() {
  if (config_.text_renderer == Dx12TextRendererMode::atlas) {
    return Status::success();
  }
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
    const HRESULT hr = impl_->dwrite_factory->CreateTextFormat(
        family.c_str(),
        nullptr,
        to_dwrite_weight(descriptor.weight),
        to_dwrite_style(descriptor.style),
        DWRITE_FONT_STRETCH_NORMAL,
        descriptor.size > 0.0f ? descriptor.size : config_.theme.body_text_size,
        locale.c_str(),
        &format);
    if (FAILED(hr)) {
      return Status::backend_error("Dx12Backend failed to create a registered text format: " + hex_hr(hr));
    }
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    impl_->font_formats[key] = std::move(format);
  }
  return Status::success();
}

Status Dx12Backend::upload_text_atlas(ID3D12GraphicsCommandList* command_list) {
  if (!impl_) {
    return Status::success();
  }
  return ::igr::backends::upload_text_atlas(*impl_, command_list);
}

Status Dx12Backend::ensure_text_bitmap_surface(std::uint32_t width, std::uint32_t height) {
  return ::igr::backends::ensure_text_bitmap_surface(*impl_, width, height);
}

Status Dx12Backend::ensure_text_atlas_resources(std::uint32_t width, std::uint32_t height) {
  Status status = ensure_text_atlas_texture(*impl_, width, height);
  if (!status) {
    return status;
  }
  return ensure_text_atlas_upload_buffer(*impl_);
}

bool Dx12Backend::should_use_text_atlas(const Dx12FrameBinding* active_binding) const noexcept {
  if (config_.text_renderer != Dx12TextRendererMode::atlas) {
    return false;
  }
  if (active_binding != nullptr && active_binding->host_sets_descriptor_heaps) {
    return false;
  }
  return true;
}

Status Dx12Backend::populate_text_atlas(const Dx12FrameBinding* active_binding) {
  if (!impl_ || impl_->device == nullptr || impl_->scratch_scene.labels.empty()) {
    return Status::success();
  }
  if (!should_use_text_atlas(active_binding)) {
    if (active_binding != nullptr && active_binding->host_sets_descriptor_heaps) {
      return Status::invalid_argument(
          "Dx12Backend atlas text rendering requires backend-managed descriptor heaps in host-managed mode.");
    }
    return Status::success();
  }
  return append_text_atlas_quads(*impl_, config_, impl_->scratch_scene.quads);
}

Status Dx12Backend::realize_registered_shaders() {
  if (impl_->device == nullptr || !impl_->root_signature) {
    return Status::success();
  }

  static constexpr D3D12_INPUT_ELEMENT_DESC kInputLayout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };

  for (const auto& [key, descriptor] : impl_->shader_descriptors) {
    if (impl_->shaders.contains(key)) {
      continue;
    }

    shaders::CompiledProgram compiled_program;
    Status status = shaders::compile_program(descriptor, {.debug_info = config_.enable_debug_layer}, &compiled_program);
    if (!status) {
      return Status::backend_error("Dx12Backend failed to compile shader '" + key + "': " + status.message());
    }

    D3D12_BLEND_DESC blend_desc{};
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    switch (descriptor.blend_mode) {
      case ShaderBlendMode::opaque:
        blend_desc.RenderTarget[0].BlendEnable = FALSE;
        break;
      case ShaderBlendMode::additive:
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        break;
      case ShaderBlendMode::alpha:
        blend_desc.RenderTarget[0].BlendEnable = TRUE;
        blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        break;
    }

    D3D12_RASTERIZER_DESC rasterizer_desc{};
    rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizer_desc.DepthClipEnable = TRUE;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
    pipeline_desc.pRootSignature = impl_->root_signature.Get();
    pipeline_desc.InputLayout = {kInputLayout, static_cast<UINT>(std::size(kInputLayout))};
    pipeline_desc.BlendState = blend_desc;
    pipeline_desc.RasterizerState = rasterizer_desc;
    pipeline_desc.DepthStencilState.DepthEnable = FALSE;
    pipeline_desc.DepthStencilState.StencilEnable = FALSE;
    pipeline_desc.SampleMask = UINT_MAX;
    pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline_desc.NumRenderTargets = 1;
    pipeline_desc.RTVFormats[0] = kRenderTargetFormat;
    pipeline_desc.SampleDesc.Count = 1;

    const std::vector<std::uint8_t>& vertex_bytecode =
        compiled_program.has_custom_vertex ? compiled_program.vertex.bytecode : impl_->default_vs_bytecode;
    pipeline_desc.VS = {vertex_bytecode.data(), vertex_bytecode.size()};
    pipeline_desc.PS = {compiled_program.pixel.bytecode.data(), compiled_program.pixel.bytecode.size()};

    Dx12CompiledShader runtime_shader;
    runtime_shader.descriptor = descriptor;
    const HRESULT hr = impl_->device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&runtime_shader.pipeline_state));
    if (FAILED(hr)) {
      return Status::backend_error("Dx12Backend failed to create pipeline state for shader '" + key + "': " + hex_hr(hr));
    }
    impl_->shaders[key] = std::move(runtime_shader);
  }

  return Status::success();
}

Status Dx12Backend::rebuild_text_targets() {
  if (!owns_device_objects_ || impl_->device == nullptr || impl_->queue == nullptr) {
    reset_text_interop_state(*impl_);
    return Status::success();
  }
  if (!impl_->text_interop_available || !impl_->on12_device || !impl_->d2d_context) {
    const Status interop_status = create_text_interop();
    if (!interop_status || !impl_->text_interop_available || !impl_->on12_device || !impl_->d2d_context) {
      return Status::success();
    }
  } else {
    clear_text_targets(*impl_);
  }

  auto build_text_targets = [&]() -> Status {
    const D3D11_RESOURCE_FLAGS resource_flags{D3D11_BIND_RENDER_TARGET};
    for (auto& frame : impl_->frames) {
      if (!frame.back_buffer) {
        continue;
      }
      HRESULT hr = impl_->on12_device->CreateWrappedResource(
          frame.back_buffer.Get(),
          &resource_flags,
          D3D12_RESOURCE_STATE_RENDER_TARGET,
          D3D12_RESOURCE_STATE_PRESENT,
          IID_PPV_ARGS(&frame.wrapped_back_buffer));
      if (FAILED(hr)) {
        return Status::backend_error("Dx12Backend failed to create a wrapped D3D11On12 back buffer: " + hex_hr(hr));
      }

      ComPtr<IDXGISurface> surface;
      hr = frame.wrapped_back_buffer.As(&surface);
      if (FAILED(hr)) {
        return Status::backend_error("Dx12Backend failed to query the wrapped DXGI surface: " + hex_hr(hr));
      }

      const auto bitmap_properties = D2D1::BitmapProperties1(
          D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
          D2D1::PixelFormat(kRenderTargetFormat, D2D1_ALPHA_MODE_PREMULTIPLIED),
          96.0f,
          96.0f);
      hr = impl_->d2d_context->CreateBitmapFromDxgiSurface(surface.Get(), &bitmap_properties, &frame.text_bitmap);
      if (FAILED(hr)) {
        return Status::backend_error("Dx12Backend failed to create a Direct2D bitmap target for the wrapped back buffer: " + hex_hr(hr));
      }
    }
    return Status::success();
  };

  Status status = build_text_targets();
  if (status) {
    return status;
  }

  reset_text_interop_state(*impl_);
  const Status retry_interop_status = create_text_interop();
  if (!retry_interop_status || !impl_->text_interop_available) {
    return Status::success();
  }

  status = build_text_targets();
  if (!status) {
    reset_text_interop_state(*impl_);
    return Status::success();
  }
  return Status::success();
}

Status Dx12Backend::create_device_objects() {
  if (impl_->device == nullptr) {
    return Status::invalid_argument("Dx12Backend cannot create device objects without a device.");
  }

  D3D12_DESCRIPTOR_RANGE range{};
  range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  range.NumDescriptors = 1;
  range.BaseShaderRegister = 0;
  range.OffsetInDescriptorsFromTableStart = 0;
  D3D12_ROOT_PARAMETER parameter{};
  parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameter.DescriptorTable.NumDescriptorRanges = 1;
  parameter.DescriptorTable.pDescriptorRanges = &range;
  parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0] = parameter;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[1].Descriptor.ShaderRegister = 0;
  parameters[1].Descriptor.RegisterSpace = 0;
  parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_STATIC_SAMPLER_DESC sampler{};
  sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  sampler.MinLOD = 0.0f;
  sampler.MaxLOD = D3D12_FLOAT32_MAX;
  sampler.ShaderRegister = 0;
  sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC root_signature_desc{};
  root_signature_desc.NumParameters = static_cast<UINT>(std::size(parameters));
  root_signature_desc.pParameters = parameters;
  root_signature_desc.NumStaticSamplers = 1;
  root_signature_desc.pStaticSamplers = &sampler;
  root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
  ComPtr<ID3DBlob> signature_blob;
  ComPtr<ID3DBlob> error_blob;
  HRESULT hr = D3D12SerializeRootSignature(&root_signature_desc, D3D_ROOT_SIGNATURE_VERSION_1, &signature_blob, &error_blob);
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to serialize the root signature: " + hex_hr(hr));
  }
  hr = impl_->device->CreateRootSignature(0, signature_blob->GetBufferPointer(), signature_blob->GetBufferSize(), IID_PPV_ARGS(&impl_->root_signature));
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to create the root signature: " + hex_hr(hr));
  }

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
  hr = D3DCompile(kVs, std::strlen(kVs), nullptr, nullptr, nullptr, "main", "vs_5_0", flags, 0, &vs_blob, &error_blob);
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to compile the vertex shader.");
  }
  hr = D3DCompile(kSolidPs, std::strlen(kSolidPs), nullptr, nullptr, nullptr, "main", "ps_5_0", flags, 0, &solid_ps_blob, &error_blob);
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to compile the solid pixel shader.");
  }
  hr = D3DCompile(kTexturedPs, std::strlen(kTexturedPs), nullptr, nullptr, nullptr, "main", "ps_5_0", flags, 0, &textured_ps_blob, &error_blob);
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to compile the textured pixel shader.");
  }
  impl_->default_vs_bytecode.resize(vs_blob->GetBufferSize());
  std::memcpy(impl_->default_vs_bytecode.data(), vs_blob->GetBufferPointer(), vs_blob->GetBufferSize());
  const D3D12_INPUT_ELEMENT_DESC input_layout[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
  };
  D3D12_BLEND_DESC blend_desc{};
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  D3D12_RASTERIZER_DESC rasterizer_desc{};
  rasterizer_desc.FillMode = D3D12_FILL_MODE_SOLID;
  rasterizer_desc.CullMode = D3D12_CULL_MODE_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline_desc{};
  pipeline_desc.pRootSignature = impl_->root_signature.Get();
  pipeline_desc.InputLayout = {input_layout, static_cast<UINT>(std::size(input_layout))};
  pipeline_desc.BlendState = blend_desc;
  pipeline_desc.RasterizerState = rasterizer_desc;
  pipeline_desc.DepthStencilState.DepthEnable = FALSE;
  pipeline_desc.DepthStencilState.StencilEnable = FALSE;
  pipeline_desc.SampleMask = UINT_MAX;
  pipeline_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pipeline_desc.NumRenderTargets = 1;
  pipeline_desc.RTVFormats[0] = kRenderTargetFormat;
  pipeline_desc.SampleDesc.Count = 1;
  pipeline_desc.VS = {vs_blob->GetBufferPointer(), vs_blob->GetBufferSize()};
  pipeline_desc.PS = {solid_ps_blob->GetBufferPointer(), solid_ps_blob->GetBufferSize()};
  hr = impl_->device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&impl_->solid_pso));
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to create the solid pipeline state: " + hex_hr(hr));
  }
  pipeline_desc.PS = {textured_ps_blob->GetBufferPointer(), textured_ps_blob->GetBufferSize()};
  hr = impl_->device->CreateGraphicsPipelineState(&pipeline_desc, IID_PPV_ARGS(&impl_->textured_pso));
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to create the textured pipeline state: " + hex_hr(hr));
  }
  const Status shader_status = realize_registered_shaders();
  if (!shader_status) {
    return shader_status;
  }
  return Status::success();
}

void Dx12Backend::reset_device_objects(bool clear_textures) noexcept {
  clear_frame_binding();
  clear_owned_frame_state(*impl_);
  clear_text_targets(*impl_);
  reset_text_bitmap_state(*impl_);
  reset_text_atlas_state(*impl_);
  if (impl_->vertex_buffer && impl_->vertex_buffer_mapped != nullptr) {
    impl_->vertex_buffer->Unmap(0, nullptr);
    impl_->vertex_buffer_mapped = nullptr;
  }
  if (impl_->shader_constant_buffer && impl_->shader_constant_buffer_mapped != nullptr) {
    impl_->shader_constant_buffer->Unmap(0, nullptr);
    impl_->shader_constant_buffer_mapped = nullptr;
  }
  impl_->vertex_buffer.Reset();
  impl_->shader_constant_buffer.Reset();
  impl_->vertex_capacity = 0;
  impl_->shader_constant_capacity = 0;
  impl_->solid_pso.Reset();
  impl_->textured_pso.Reset();
  impl_->root_signature.Reset();
  impl_->default_vs_bytecode.clear();
  detail::release_storage(impl_->default_vs_bytecode);
  impl_->shaders.clear();
  impl_->font_formats.clear();
  impl_->title_format.Reset();
  impl_->body_format.Reset();
  impl_->center_format.Reset();
  impl_->d2d_brush.Reset();
  impl_->d2d_context.Reset();
  impl_->d2d_device.Reset();
  impl_->d2d_factory.Reset();
  impl_->dwrite_factory.Reset();
  impl_->on12_device.Reset();
  impl_->interop_context.Reset();
  impl_->interop_device.Reset();
  impl_->text_interop_available = false;
  impl_->owned_command_list_executed = false;
  if (clear_textures) {
    impl_->textures.clear();
    detail::release_storage(impl_->textures);
  }
  release_transient_storage();
}

Dx12Backend::Dx12Backend(Dx12BackendConfig config) : config_(config), viewport_(config.initial_viewport), impl_(std::make_unique<Impl>()) {}

Dx12Backend::~Dx12Backend() {
  shutdown();
}

BackendKind Dx12Backend::kind() const noexcept { return BackendKind::dx12; }
std::string_view Dx12Backend::name() const noexcept { return "DirectX 12"; }

BackendCapabilities Dx12Backend::capabilities() const noexcept {
  return {.debug_layer = config_.enable_debug_layer,
          .user_textures = true,
          .user_shaders = true,
          .docking = true,
          .multi_viewport = false,
          .manual_host_binding = true,
          .host_state_restore = false,
          .injected_overlay = true};
}

Status Dx12Backend::initialize() {
  const auto initialize_started = std::chrono::steady_clock::now();
  if (initialized_) {
    return Status::success();
  }
  const bool host_managed = config_.host.host_mode != HostMode::owned_window;
  if (host_managed) {
    if (config_.device == nullptr || config_.command_queue == nullptr || config_.swap_chain == nullptr) {
      return Status::invalid_argument("Dx12Backend requires a device, command queue, and swap chain.");
    }
    impl_->device = config_.device;
    impl_->queue = config_.command_queue;
    impl_->swap_chain = config_.swap_chain;
    owns_device_objects_ = false;
  } else {
    if (config_.window_handle == nullptr) {
      return Status::invalid_argument("Dx12Backend requires a window handle in owned-window mode.");
    }
    if (viewport_.width == 0 || viewport_.height == 0) {
      RECT rect{};
      GetClientRect(config_.window_handle, &rect);
      viewport_.width = std::max(static_cast<std::uint32_t>(rect.right - rect.left), 1u);
      viewport_.height = std::max(static_cast<std::uint32_t>(rect.bottom - rect.top), 1u);
    }
    Status status = create_factory(impl_->factory.GetAddressOf(), config_.enable_debug_layer);
    if (!status) {
      shutdown();
      return status;
    }
    status = create_owned_device(impl_->factory.Get(), impl_->owned_device.GetAddressOf());
    if (!status) {
      shutdown();
      return status;
    }
    impl_->device = impl_->owned_device.Get();
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = impl_->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&impl_->owned_queue));
    if (FAILED(hr)) {
      shutdown();
      return Status::backend_error("Dx12Backend failed to create the command queue: " + hex_hr(hr));
    }
    impl_->queue = impl_->owned_queue.Get();
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
    swap_chain_desc.BufferCount = kFrameCount;
    swap_chain_desc.Width = viewport_.width;
    swap_chain_desc.Height = viewport_.height;
    swap_chain_desc.Format = kRenderTargetFormat;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap_chain1;
    hr = impl_->factory->CreateSwapChainForHwnd(impl_->queue, config_.window_handle, &swap_chain_desc, nullptr, nullptr, &swap_chain1);
    if (FAILED(hr)) {
      shutdown();
      return Status::backend_error("Dx12Backend failed to create the swap chain: " + hex_hr(hr));
    }
    impl_->factory->MakeWindowAssociation(config_.window_handle, DXGI_MWA_NO_ALT_ENTER);
    hr = swap_chain1.As(&impl_->owned_swap_chain);
    if (FAILED(hr)) {
      shutdown();
      return Status::backend_error("Dx12Backend failed to query IDXGISwapChain3: " + hex_hr(hr));
    }
    impl_->swap_chain = impl_->owned_swap_chain.Get();
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
    rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_desc.NumDescriptors = kFrameCount;
    hr = impl_->device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&impl_->rtv_heap));
    if (FAILED(hr)) {
      shutdown();
      return Status::backend_error("Dx12Backend failed to create the RTV descriptor heap: " + hex_hr(hr));
    }
    impl_->rtv_descriptor_size = impl_->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT index = 0; index < kFrameCount; ++index) {
      hr = impl_->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&impl_->frames[index].allocator));
      if (FAILED(hr)) {
        shutdown();
        return Status::backend_error("Dx12Backend failed to create a command allocator: " + hex_hr(hr));
      }
    }
    hr = impl_->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, impl_->frames[0].allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&impl_->owned_command_list));
    if (FAILED(hr)) {
      shutdown();
      return Status::backend_error("Dx12Backend failed to create the graphics command list: " + hex_hr(hr));
    }
    impl_->owned_command_list->Close();
    hr = impl_->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&impl_->owned_fence));
    if (FAILED(hr)) {
      shutdown();
      return Status::backend_error("Dx12Backend failed to create the fence: " + hex_hr(hr));
    }
    impl_->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (impl_->fence_event == nullptr) {
      shutdown();
      return Status::backend_error("Dx12Backend failed to create the fence event.");
    }
    owns_device_objects_ = true;
  }

  if (impl_->device != nullptr) {
    const Status device_object_status = create_device_objects();
    if (!device_object_status) {
      shutdown();
      return device_object_status;
    }
  }

  if (owns_device_objects_) {
    const D3D12_CPU_DESCRIPTOR_HANDLE heap_start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
    for (UINT index = 0; index < kFrameCount; ++index) {
      HRESULT hr = impl_->swap_chain->GetBuffer(index, IID_PPV_ARGS(&impl_->frames[index].back_buffer));
      if (FAILED(hr)) {
        shutdown();
        return Status::backend_error("Dx12Backend failed to access a swap-chain back buffer: " + hex_hr(hr));
      }
      D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_start;
      rtv.ptr += static_cast<SIZE_T>(index) * impl_->rtv_descriptor_size;
      impl_->frames[index].rtv = rtv;
      impl_->device->CreateRenderTargetView(impl_->frames[index].back_buffer.Get(), nullptr, rtv);
    }
    const Status text_target_status = rebuild_text_targets();
    if (!text_target_status) {
      shutdown();
      return text_target_status;
    }
  }

  initialized_ = true;
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "initialize",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - initialize_started).count()));
    refresh_telemetry();
  }
  return Status::success();
}

void Dx12Backend::invalidate_back_buffer_resources() noexcept {
  if (!impl_) {
    return;
  }
  clear_owned_frame_submission_state();
  clear_text_targets(*impl_);
  for (auto& frame : impl_->frames) {
    frame.back_buffer.Reset();
    frame.fence_value = 0;
  }
}

Status Dx12Backend::resize(ExtentU viewport) {
  const auto resize_started = std::chrono::steady_clock::now();
  if (!initialized_) {
    return Status::not_ready("Dx12Backend must be initialized before resize.");
  }
  if (!impl_) {
    return Status::not_ready("Dx12Backend cannot resize before backend state is allocated.");
  }
  if (same_extent(viewport_, viewport) && (!owns_device_objects_ || impl_->swap_chain != nullptr)) {
    bool resources_ready = !owns_device_objects_ || (impl_->rtv_heap != nullptr && impl_->frames[0].back_buffer != nullptr);
    if (resources_ready) {
      return Status::success();
    }
  }
  viewport_ = viewport;
  if (!owns_device_objects_) {
    if (viewport_.width == 0 || viewport_.height == 0) {
      clear_frame_binding();
    }
    if (config_.diagnostics.enabled) {
      record_scope(impl_->scope_totals,
                   "resize",
                   static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - resize_started).count()));
      refresh_telemetry();
    }
    return Status::success();
  }
  if (impl_->device == nullptr || impl_->swap_chain == nullptr || impl_->rtv_heap == nullptr) {
    return Status::not_ready("Dx12Backend cannot resize before owned device objects are ready.");
  }
  if (viewport_.width == 0 || viewport_.height == 0) {
    const Status idle_status = wait_for_gpu_idle(*impl_);
    if (!idle_status) {
      return idle_status;
    }
    invalidate_back_buffer_resources();
    if (config_.diagnostics.enabled) {
      record_scope(impl_->scope_totals,
                   "resize",
                   static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - resize_started).count()));
      refresh_telemetry();
    }
    return Status::success();
  }
  Status status = wait_for_gpu_idle(*impl_);
  if (!status) {
    return status;
  }
  clear_owned_frame_submission_state();
  reset_device_objects(false);
  impl_->owned_command_list.Reset();
  for (auto& frame : impl_->frames) {
    if (frame.allocator) {
      frame.allocator->Reset();
      frame.fence_value = 0;
    }
  }
  invalidate_back_buffer_resources();
  const HRESULT hr = impl_->swap_chain->ResizeBuffers(0, viewport_.width, viewport_.height, kRenderTargetFormat, 0);
  if (FAILED(hr)) {
    return Status::backend_error(device_error_message(impl_->device, "Dx12Backend failed to resize swap-chain buffers", hr));
  }
  const D3D12_CPU_DESCRIPTOR_HANDLE heap_start = impl_->rtv_heap->GetCPUDescriptorHandleForHeapStart();
  for (UINT index = 0; index < kFrameCount; ++index) {
    HRESULT buffer_hr = impl_->swap_chain->GetBuffer(index, IID_PPV_ARGS(&impl_->frames[index].back_buffer));
    if (FAILED(buffer_hr)) {
      return Status::backend_error(device_error_message(impl_->device, "Dx12Backend failed to reacquire a resized back buffer", buffer_hr));
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap_start;
    rtv.ptr += static_cast<SIZE_T>(index) * impl_->rtv_descriptor_size;
    impl_->frames[index].rtv = rtv;
    impl_->device->CreateRenderTargetView(impl_->frames[index].back_buffer.Get(), nullptr, rtv);
  }
  status = create_device_objects();
  if (!status) {
    return status;
  }
  const HRESULT command_list_hr = impl_->device->CreateCommandList(
      0,
      D3D12_COMMAND_LIST_TYPE_DIRECT,
      impl_->frames[0].allocator.Get(),
      nullptr,
      IID_PPV_ARGS(&impl_->owned_command_list));
  if (FAILED(command_list_hr)) {
    return Status::backend_error(device_error_message(impl_->device, "Dx12Backend failed to recreate the graphics command list after resize", command_list_hr));
  }
  impl_->owned_command_list->Close();
  Status final_status = impl_->text_interop_available ? rebuild_text_targets() : Status::success();
  if (final_status && config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "resize",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - resize_started).count()));
    refresh_telemetry();
  }
  return final_status;
}

Status Dx12Backend::render(const FrameDocument& document) {
  const auto render_started = std::chrono::steady_clock::now();
  if (!initialized_) {
    return Status::not_ready("Dx12Backend must be initialized before render.");
  }
  if (!impl_ || impl_->device == nullptr) {
    return Status::not_ready("Dx12Backend cannot render before the device is ready.");
  }
  impl_->stats = {};
  impl_->stats.frame_index = document.info.frame_index;
  if (viewport_.width == 0 || viewport_.height == 0) {
    last_widget_count_ = document.widget_count();
    return Status::success();
  }
  const Dx12FrameBinding* active_binding = nullptr;
  if (config_.host.host_mode != HostMode::owned_window) {
    if (!has_frame_binding_) {
      return Status::invalid_argument("Dx12Backend host-managed rendering requires a frame binding before render().");
    }
    const Status binding_status = validate_frame_binding(frame_binding_);
    if (!binding_status) {
      return binding_status;
    }
    active_binding = &frame_binding_;
  }
  const Status texture_status = validate_document_textures(document, impl_->textures, impl_->image_resources, impl_->shader_descriptors, active_binding);
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
  const bool atlas_supported_for_frame = config_.text_renderer == Dx12TextRendererMode::atlas &&
                                         (active_binding == nullptr || !active_binding->host_sets_descriptor_heaps);
  bool use_text_atlas = false;
  if (!impl_->scratch_scene.labels.empty() && atlas_supported_for_frame) {
    const Status atlas_status = populate_text_atlas(active_binding);
    if (atlas_status) {
      use_text_atlas = true;
      impl_->textless_frame_streak = 0;
      if (impl_->text_interop_available) {
        reset_text_interop_state(*impl_);
      }
    } else if (owns_device_objects_) {
      reset_text_atlas_state(*impl_);
      use_text_atlas = false;
    } else {
      return atlas_status;
    }
  } else if (impl_->scratch_scene.labels.empty()) {
    if (++impl_->textless_frame_streak >= config_.text_interop_idle_frame_threshold) {
      if (impl_->text_interop_available) {
        reset_text_interop_state(*impl_);
      }
      reset_text_atlas_state(*impl_);
      reset_text_bitmap_state(*impl_);
      impl_->textless_frame_streak = 0;
    }
  } else {
    impl_->textless_frame_streak = 0;
  }
  build_batches(impl_->scratch_scene.quads, viewport_, impl_->scratch_vertices, impl_->scratch_batches);
  for (const auto& root : document.roots) {
    accumulate_widget_stats(root, impl_->stats);
  }
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
  impl_->stats.scene_build_microseconds =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - build_started).count());
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals, "scene_build", impl_->stats.scene_build_microseconds);
  }
  Status status = ensure_vertex_buffer(*impl_, impl_->scratch_vertices.size());
  if (!status) {
    return status;
  }
  status = ensure_shader_constant_buffer(*impl_, impl_->stats.shader_batch_count);
  if (!status) {
    return status;
  }
  const auto upload_started = std::chrono::steady_clock::now();
  if (!impl_->scratch_vertices.empty()) {
    std::memcpy(impl_->vertex_buffer_mapped, impl_->scratch_vertices.data(), impl_->scratch_vertices.size() * sizeof(QuadVertex));
  }
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "vertex_upload",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - upload_started).count()));
  }

  const auto submit_started = std::chrono::steady_clock::now();
  const auto render_text_for_owned_frame = [&](std::uint32_t owned_frame_index) -> Status {
    if (!impl_->text_interop_available || impl_->scratch_scene.labels.empty()) {
      return Status::success();
    }
    if (owned_frame_index >= impl_->frames.size()) {
      return Status::invalid_argument("Dx12Backend text rendering received an invalid owned-frame index.");
    }
    auto& text_frame = impl_->frames[owned_frame_index];
    if (!text_frame.wrapped_back_buffer || !text_frame.text_bitmap || !impl_->on12_device || !impl_->d2d_context || !impl_->d2d_brush) {
      return Status::success();
    }
    const auto text_pass_started = std::chrono::steady_clock::now();

    ID3D11Resource* wrapped_resources[] = {text_frame.wrapped_back_buffer.Get()};
    impl_->on12_device->AcquireWrappedResources(wrapped_resources, 1);
    impl_->d2d_context->SetTarget(text_frame.text_bitmap.Get());
    impl_->d2d_context->BeginDraw();
    for (const auto& item : impl_->scratch_scene.labels) {
      impl_->d2d_brush->SetColor(to_d2d_color(item.color));
      IDWriteTextFormat* format = impl_->body_format.Get();
      if (!item.font_key.empty()) {
        const auto font_it = impl_->font_formats.find(item.font_key);
        if (font_it != impl_->font_formats.end()) {
          format = font_it->second.Get();
        }
      }
      if (format == impl_->body_format.Get()) {
        if (item.style == TextStyle::title) {
          format = impl_->title_format.Get();
        }
        if (item.style == TextStyle::center) {
          format = impl_->center_format.Get();
        }
      }
      if (item.clip_rect.has_value()) {
        impl_->d2d_context->PushAxisAlignedClip(d2d_rect_from_clip(*item.clip_rect), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      }
      impl_->d2d_context->DrawText(item.text->c_str(), static_cast<UINT32>(item.text->size()), format, item.rect, impl_->d2d_brush.Get());
      if (item.clip_rect.has_value()) {
        impl_->d2d_context->PopAxisAlignedClip();
      }
    }
    const HRESULT draw_hr = impl_->d2d_context->EndDraw();
    impl_->d2d_context->SetTarget(nullptr);
    impl_->on12_device->ReleaseWrappedResources(wrapped_resources, 1);
    impl_->interop_context->Flush();
    if (draw_hr == D2DERR_RECREATE_TARGET) {
      return impl_->text_interop_available ? rebuild_text_targets() : Status::success();
    }
    if (FAILED(draw_hr)) {
      return Status::backend_error("Dx12Backend DirectWrite text pass failed: " + hex_hr(draw_hr));
    }
    if (config_.diagnostics.enabled) {
      record_scope(impl_->scope_totals,
                   "text_pass",
                   static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - text_pass_started).count()));
    }
    return Status::success();
  };

  auto record_batches = [&](ID3D12GraphicsCommandList* command_list, const Dx12FrameBinding& binding) -> Status {
    if (command_list == nullptr) {
      return Status::invalid_argument("Dx12Backend requires a valid command list for command recording.");
    }

    if (!binding.host_transitions_render_target && binding.render_target_state_before != D3D12_RESOURCE_STATE_RENDER_TARGET) {
      D3D12_RESOURCE_BARRIER to_render_target{};
      to_render_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      to_render_target.Transition.pResource = binding.render_target;
      to_render_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      to_render_target.Transition.StateBefore = binding.render_target_state_before;
      to_render_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
      command_list->ResourceBarrier(1, &to_render_target);
    }

    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(viewport_.width), static_cast<float>(viewport_.height), 0.0f, 1.0f};
    const D3D12_RECT full_scissor = scissor_from_clip(std::nullopt, viewport_);
    command_list->RSSetViewports(1, &viewport);
    command_list->RSSetScissorRects(1, &full_scissor);
    if (!binding.host_sets_render_target) {
      command_list->OMSetRenderTargets(1, &binding.render_target_view, FALSE, nullptr);
    }
    if (binding.clear_target) {
      command_list->ClearRenderTargetView(binding.render_target_view, config_.clear_color.data(), 0, nullptr);
    }
    command_list->SetGraphicsRootSignature(impl_->root_signature.Get());
    command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* current_heap = nullptr;
    if (binding.host_sets_descriptor_heaps && binding.shader_visible_srv_heap != nullptr) {
      current_heap = binding.shader_visible_srv_heap;
    }

    if (!impl_->scratch_vertices.empty()) {
      D3D12_VERTEX_BUFFER_VIEW vertex_view{};
      vertex_view.BufferLocation = impl_->vertex_buffer->GetGPUVirtualAddress();
      vertex_view.SizeInBytes = static_cast<UINT>(impl_->scratch_vertices.size() * sizeof(QuadVertex));
      vertex_view.StrideInBytes = sizeof(QuadVertex);
      command_list->IASetVertexBuffers(0, 1, &vertex_view);

      std::size_t shader_batch_index = 0;
      for (const auto& batch : impl_->scratch_batches) {
        const D3D12_RECT scissor = scissor_from_clip(batch.clip_rect, viewport_);
        command_list->RSSetScissorRects(1, &scissor);
        if (batch.shader_key.empty() && batch.texture_key.empty()) {
          command_list->SetPipelineState(impl_->solid_pso.Get());
        } else if (batch.shader_key.empty()) {
          const auto texture_it = impl_->textures.find(batch.texture_key);
          if (texture_it == impl_->textures.end()) {
            return Status::invalid_argument("Dx12Backend is missing the texture binding for batch '" + std::string(batch.texture_key) + "'.");
          }
          const auto& texture_binding = texture_it->second;
          if (!binding.host_sets_descriptor_heaps && texture_binding.heap != current_heap) {
            current_heap = texture_binding.heap;
          }
          if (current_heap != nullptr) {
            command_list->SetDescriptorHeaps(1, &current_heap);
          }
          command_list->SetPipelineState(impl_->textured_pso.Get());
          command_list->SetGraphicsRootDescriptorTable(0, texture_binding.gpu_descriptor);
        } else {
          const auto shader_it = impl_->shaders.find(batch.shader_key);
          if (shader_it == impl_->shaders.end()) {
            return Status::invalid_argument("Dx12Backend is missing the realized shader '" + std::string(batch.shader_key) + "'.");
          }
          const Dx12CompiledShader& shader = shader_it->second;
          command_list->SetPipelineState(shader.pipeline_state.Get());

          ShaderConstants constants;
          constants.tint = batch.tint;
          constants.params = batch.params;
          constants.rect = {batch.bounds.origin.x, batch.bounds.origin.y, batch.bounds.extent.x, batch.bounds.extent.y};
          constants.viewport_and_time = {static_cast<float>(viewport_.width),
                                         static_cast<float>(viewport_.height),
                                         static_cast<float>(document.info.time_seconds),
                                         static_cast<float>(document.info.delta_seconds)};
          constants.frame_data = {static_cast<float>(document.info.frame_index), shader.descriptor.samples_texture ? 1.0f : 0.0f, 0.0f, 0.0f};

          const std::size_t constant_offset = align_constant_buffer_size(sizeof(ShaderConstants)) * shader_batch_index++;
          std::memcpy(impl_->shader_constant_buffer_mapped + constant_offset, &constants, sizeof(constants));
          command_list->SetGraphicsRootConstantBufferView(
              1, impl_->shader_constant_buffer->GetGPUVirtualAddress() + static_cast<UINT64>(constant_offset));

          if (shader.descriptor.samples_texture) {
            const auto texture_it = impl_->textures.find(batch.texture_key);
            if (texture_it == impl_->textures.end()) {
              return Status::invalid_argument("Dx12Backend is missing the texture binding for shader batch '" + std::string(batch.texture_key) + "'.");
            }
            const auto& texture_binding = texture_it->second;
            if (!binding.host_sets_descriptor_heaps && texture_binding.heap != current_heap) {
              current_heap = texture_binding.heap;
            }
            if (current_heap != nullptr) {
              command_list->SetDescriptorHeaps(1, &current_heap);
            }
            command_list->SetGraphicsRootDescriptorTable(0, texture_binding.gpu_descriptor);
          }
        }
        command_list->DrawInstanced(static_cast<UINT>(batch.vertex_count), 1, static_cast<UINT>(batch.start_vertex), 0);
      }
    }

    if (!binding.host_transitions_render_target && binding.render_target_state_after != D3D12_RESOURCE_STATE_RENDER_TARGET) {
      D3D12_RESOURCE_BARRIER to_present{};
      to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      to_present.Transition.pResource = binding.render_target;
      to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
      to_present.Transition.StateAfter = binding.render_target_state_after;
      command_list->ResourceBarrier(1, &to_present);
    }

    return Status::success();
  };

  const bool wants_interop_text = !use_text_atlas && config_.text_renderer == Dx12TextRendererMode::interop;

  if (!owns_device_objects_) {
    if (use_text_atlas) {
      status = upload_text_atlas(frame_binding_.command_list);
      if (!status) {
        return status;
      }
      impl_->textures[std::string(kInternalTextAtlasKey)] = {
          .heap = impl_->text_atlas_heap.Get(),
          .cpu_descriptor = impl_->text_atlas_heap->GetCPUDescriptorHandleForHeapStart(),
          .gpu_descriptor = impl_->text_atlas_heap->GetGPUDescriptorHandleForHeapStart(),
      };
    } else {
      impl_->textures.erase(std::string(kInternalTextAtlasKey));
    }
    status = record_batches(frame_binding_.command_list, frame_binding_);
    if (!status) {
      return status;
    }
    impl_->stats.render_submit_microseconds =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - submit_started).count());
    if (config_.diagnostics.enabled) {
      record_scope(impl_->scope_totals, "render_submit", impl_->stats.render_submit_microseconds);
    }
    last_widget_count_ = document.widget_count();
    trim_wide_text_cache(impl_->wide_text_cache, config_.resource_budgets);
    trim_scratch_storage(impl_->scratch_scene, impl_->scratch_vertices, impl_->scratch_batches, config_.resource_budgets);
    trim_upload_buffer(impl_->vertex_buffer,
                       impl_->vertex_buffer_mapped,
                       impl_->vertex_capacity,
                       impl_->scratch_vertices.size(),
                       config_.resource_budgets.max_retained_vertices,
                       256);
    trim_upload_buffer(impl_->shader_constant_buffer,
                       impl_->shader_constant_buffer_mapped,
                       impl_->shader_constant_capacity,
                       impl_->stats.shader_batch_count,
                       config_.resource_budgets.max_retained_shader_constants,
                       16);
    if (use_text_atlas && !impl_->text_atlas_dirty &&
        config_.text_atlas.staging_release_frame_threshold > 0 &&
        impl_->text_atlas_stable_frame_streak >= config_.text_atlas.staging_release_frame_threshold) {
      release_text_atlas_staging(*impl_);
    }
    refresh_telemetry();
    return Status::success();
  }

  if (impl_->owned_frame_pending) {
    return Status::not_ready("Dx12Backend requires present() before rendering another owned-window frame.");
  }

  const UINT frame_index = impl_->swap_chain->GetCurrentBackBufferIndex();
  auto& frame = impl_->frames[frame_index];
  if (frame.back_buffer == nullptr || frame.rtv.ptr == 0 || impl_->owned_command_list == nullptr) {
    return Status::not_ready("Dx12Backend cannot render an owned frame before back-buffer resources are ready.");
  }
  if (wants_interop_text && !impl_->scratch_scene.labels.empty() && (!impl_->text_interop_available || !frame.text_bitmap || !frame.wrapped_back_buffer)) {
    status = rebuild_text_targets();
    if (!status) {
      return status;
    }
  } else if (!wants_interop_text && impl_->text_interop_available) {
    reset_text_interop_state(*impl_);
  }
  status = wait_for_fence(*impl_, frame.fence_value);
  if (!status) {
    return status;
  }
  HRESULT hr = frame.allocator->Reset();
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to reset the command allocator: " + hex_hr(hr) +
                                 recent_debug_messages(impl_->device));
  }
  hr = impl_->owned_command_list->Reset(frame.allocator.Get(), impl_->solid_pso.Get());
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to reset the command list: " + hex_hr(hr) +
                                 recent_debug_messages(impl_->device));
  }

  Dx12FrameBinding owned_binding{};
  owned_binding.frame_index = static_cast<std::uint32_t>(document.info.frame_index);
  owned_binding.back_buffer_index = frame_index;
  owned_binding.command_list = impl_->owned_command_list.Get();
  owned_binding.render_target = frame.back_buffer.Get();
  owned_binding.render_target_view = frame.rtv;
  owned_binding.host_sets_render_target = false;
  owned_binding.host_sets_descriptor_heaps = false;
  owned_binding.host_transitions_render_target = false;
  owned_binding.render_target_state_before = D3D12_RESOURCE_STATE_PRESENT;
  const bool use_text_interop =
      wants_interop_text && impl_->text_interop_available && !impl_->scratch_scene.labels.empty() && frame.text_bitmap && frame.wrapped_back_buffer;
  owned_binding.render_target_state_after = use_text_interop ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_PRESENT;
  owned_binding.clear_target = true;

  if (use_text_atlas) {
    status = upload_text_atlas(impl_->owned_command_list.Get());
    if (!status) {
      return status;
    }
    impl_->textures[std::string(kInternalTextAtlasKey)] = {
        .heap = impl_->text_atlas_heap.Get(),
        .cpu_descriptor = impl_->text_atlas_heap->GetCPUDescriptorHandleForHeapStart(),
        .gpu_descriptor = impl_->text_atlas_heap->GetGPUDescriptorHandleForHeapStart(),
    };
  } else {
    impl_->textures.erase(std::string(kInternalTextAtlasKey));
  }

  status = record_batches(impl_->owned_command_list.Get(), owned_binding);
  if (!status) {
    return status;
  }

  hr = impl_->owned_command_list->Close();
  if (FAILED(hr)) {
    return Status::backend_error("Dx12Backend failed to close the command list: " + hex_hr(hr) +
                                 recent_debug_messages(impl_->device));
  }

  impl_->owned_command_list_executed = false;
  if (use_text_interop) {
    ID3D12CommandList* command_lists[] = {impl_->owned_command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, command_lists);
    impl_->owned_command_list_executed = true;
    status = render_text_for_owned_frame(frame_index);
    if (!status) {
      return status;
    }
  }

  impl_->owned_pending_frame_index = frame_index;
  impl_->owned_frame_pending = true;
  last_widget_count_ = document.widget_count();
  impl_->stats.render_submit_microseconds =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - submit_started).count());
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals, "render_submit", impl_->stats.render_submit_microseconds);
  }
  trim_wide_text_cache(impl_->wide_text_cache, config_.resource_budgets);
  trim_scratch_storage(impl_->scratch_scene, impl_->scratch_vertices, impl_->scratch_batches, config_.resource_budgets);
  trim_upload_buffer(impl_->vertex_buffer,
                     impl_->vertex_buffer_mapped,
                     impl_->vertex_capacity,
                     impl_->scratch_vertices.size(),
                     config_.resource_budgets.max_retained_vertices,
                     256);
  trim_upload_buffer(impl_->shader_constant_buffer,
                     impl_->shader_constant_buffer_mapped,
                     impl_->shader_constant_capacity,
                     impl_->stats.shader_batch_count,
                     config_.resource_budgets.max_retained_shader_constants,
                     16);
  if (use_text_atlas && !impl_->text_atlas_dirty &&
      config_.text_atlas.staging_release_frame_threshold > 0 &&
      impl_->text_atlas_stable_frame_streak >= config_.text_atlas.staging_release_frame_threshold) {
    release_text_atlas_staging(*impl_);
  }
  refresh_telemetry();
  return Status::success();
}

Status Dx12Backend::present() {
  const auto present_started = std::chrono::steady_clock::now();
  if (!initialized_) {
    return Status::not_ready("Dx12Backend must be initialized before present.");
  }
  if (config_.host.presentation_mode == PresentationMode::host_managed || !owns_device_objects_) {
    return Status::success();
  }
  if (!impl_ || impl_->queue == nullptr || impl_->swap_chain == nullptr || impl_->owned_fence == nullptr) {
    return Status::not_ready("Dx12Backend cannot present before owned device objects are ready.");
  }
  if (!impl_->owned_frame_pending) {
    return Status::success();
  }
  if (!impl_->owned_command_list_executed) {
    if (!impl_->owned_command_list) {
      clear_owned_frame_state(*impl_);
      return Status::not_ready("Dx12Backend cannot present without an owned graphics command list.");
    }
    ID3D12CommandList* command_lists[] = {impl_->owned_command_list.Get()};
    impl_->queue->ExecuteCommandLists(1, command_lists);
  }
  auto& frame = impl_->frames[impl_->owned_pending_frame_index];
  const std::uint64_t fence_value = impl_->next_fence_value++;
  HRESULT hr = impl_->queue->Signal(impl_->owned_fence.Get(), fence_value);
  if (FAILED(hr)) {
    clear_owned_frame_state(*impl_);
    return Status::backend_error(device_error_message(impl_->device, "Dx12Backend failed to signal the frame fence", hr));
  }
  frame.fence_value = fence_value;
  hr = impl_->swap_chain->Present(config_.enable_vsync ? 1u : 0u, 0u);
  if (hr == DXGI_STATUS_OCCLUDED || hr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS) {
    clear_owned_frame_submission_state();
    return Status::success();
  }
  if (FAILED(hr)) {
    clear_owned_frame_submission_state();
    return Status::backend_error(device_error_message(impl_->device, "Dx12Backend failed to present the swap chain", hr));
  }
  clear_owned_frame_submission_state();
  if (config_.diagnostics.enabled) {
    record_scope(impl_->scope_totals,
                 "present",
                 static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - present_started).count()));
    refresh_telemetry();
  }
  return Status::success();
}

BackendFrameStats Dx12Backend::frame_stats() const noexcept {
  return impl_ ? impl_->stats : BackendFrameStats{};
}

BackendTelemetrySnapshot Dx12Backend::telemetry() const noexcept {
  return impl_ ? impl_->telemetry : BackendTelemetrySnapshot{};
}

void Dx12Backend::refresh_telemetry() noexcept {
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
    ComPtr<IDXGIFactory4> diagnostics_factory;
    ComPtr<IDXGIAdapter> diagnostics_adapter;
    impl_->telemetry.gpu_memory = query_gpu_memory(resolve_dxgi_adapter(impl_->device, impl_->factory.Get(), diagnostics_factory, diagnostics_adapter));
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
    resources.gpu_vertex_buffer_bytes = estimate_d3d12_resource_bytes(impl_->vertex_buffer.Get());
    resources.gpu_constant_buffer_bytes = estimate_d3d12_resource_bytes(impl_->shader_constant_buffer.Get());
    resources.gpu_text_atlas_bytes = estimate_d3d12_resource_bytes(impl_->text_atlas_texture.Get());
    resources.cpu_text_bitmap_bytes = estimate_bitmap_bytes(impl_->text_bitmap_width, impl_->text_bitmap_height);
    resources.text_interop_active = impl_->text_interop_available;
    resources.text_atlas_active = impl_->text_atlas_texture != nullptr;
    resources.total_estimated_bytes = resources.font_cache_bytes + resources.wide_text_cache_bytes + resources.scene_bytes +
                                      resources.scratch_vertex_bytes + resources.scratch_batch_bytes + resources.gpu_vertex_buffer_bytes +
                                      resources.gpu_constant_buffer_bytes + resources.gpu_text_atlas_bytes + resources.cpu_text_bitmap_bytes;
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

Status Dx12Backend::register_font(std::string_view key, const FontResourceDesc& descriptor) {
  const auto started = std::chrono::steady_clock::now();
  if (key.empty()) {
    return Status::invalid_argument("Dx12Backend font registration requires a non-empty key.");
  }
  const Status validation_status = validate_font_descriptor("Dx12Backend", descriptor);
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

void Dx12Backend::unregister_font(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->font_descriptors.erase(std::string(key));
  impl_->font_formats.erase(std::string(key));
  refresh_telemetry();
}

Status Dx12Backend::register_image(std::string_view key, const ImageResourceDesc& descriptor) {
  const auto started = std::chrono::steady_clock::now();
  if (key.empty()) {
    return Status::invalid_argument("Dx12Backend image registration requires a non-empty key.");
  }
  const Status validation_status = validate_image_descriptor("Dx12Backend", descriptor);
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

void Dx12Backend::unregister_image(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->image_resources.erase(std::string(key));
  refresh_telemetry();
}

Status Dx12Backend::register_shader(std::string_view key, const ShaderResourceDesc& descriptor) {
  const auto started = std::chrono::steady_clock::now();
  if (key.empty()) {
    return Status::invalid_argument("Dx12Backend shader registration requires a non-empty key.");
  }
  if (descriptor.pixel.source.empty()) {
    return Status::invalid_argument("Dx12Backend shader registration requires a pixel shader source.");
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

void Dx12Backend::unregister_shader(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->shader_descriptors.erase(std::string(key));
  impl_->shaders.erase(std::string(key));
  refresh_telemetry();
}

void Dx12Backend::shutdown() noexcept {
  if (impl_) {
    if (owns_device_objects_) {
      wait_for_gpu_idle(*impl_);
    }
    reset_device_objects(true);
    impl_->font_descriptors.clear();
    impl_->image_resources.clear();
    impl_->shader_descriptors.clear();
    detail::release_storage(impl_->font_descriptors);
    detail::release_storage(impl_->image_resources);
    detail::release_storage(impl_->shader_descriptors);
    detail::release_storage(impl_->font_formats);
    detail::release_storage(impl_->wide_text_cache);
    detail::release_storage(impl_->shaders);
    invalidate_back_buffer_resources();
    impl_->owned_command_list.Reset();
    impl_->owned_fence.Reset();
    if (impl_->fence_event != nullptr) {
      CloseHandle(impl_->fence_event);
      impl_->fence_event = nullptr;
    }
    impl_->rtv_heap.Reset();
    impl_->swap_chain = nullptr;
    impl_->owned_swap_chain.Reset();
    impl_->queue = nullptr;
    impl_->owned_queue.Reset();
    impl_->device = nullptr;
    impl_->owned_device.Reset();
    impl_->factory.Reset();
    impl_->owned_frame_pending = false;
    impl_->stats = {};
    impl_->telemetry = {};
    impl_->telemetry_refresh_count = 0;
    release_transient_storage();
  }
  viewport_ = {};
  last_widget_count_ = 0;
  initialized_ = false;
  owns_device_objects_ = false;
  clear_frame_binding();
}

Status Dx12Backend::bind_frame(const Dx12FrameBinding& binding) {
  if (!initialized_) {
    return Status::not_ready("Dx12Backend must be initialized before binding a frame.");
  }
  Dx12FrameBinding resolved_binding = binding;
  if (resolved_binding.command_list == nullptr) {
    resolved_binding.command_list = config_.command_list;
  }
  const Status binding_status = validate_frame_binding(resolved_binding);
  if (!binding_status) {
    return binding_status;
  }
  frame_binding_ = resolved_binding;
  has_frame_binding_ = true;
  return Status::success();
}

Status Dx12Backend::register_texture(std::string_view key, const Dx12TextureBinding& binding) {
  if (key.empty()) {
    return Status::invalid_argument("Dx12Backend texture registration requires a non-empty key.");
  }
  if (!initialized_ || impl_->device == nullptr) {
    return Status::not_ready("Dx12Backend must be initialized before registering textures.");
  }
  const Status validation_status = validate_texture_binding(binding);
  if (!validation_status) {
    return validation_status;
  }
  impl_->textures[std::string(key)] = binding;
  if (config_.diagnostics.enabled) {
    refresh_telemetry();
  }
  return Status::success();
}

void Dx12Backend::unregister_texture(std::string_view key) noexcept {
  if (!impl_ || key.empty()) {
    return;
  }
  impl_->textures.erase(std::string(key));
  refresh_telemetry();
}

void Dx12Backend::clear_frame_binding() noexcept {
  frame_binding_ = {};
  has_frame_binding_ = false;
}

void Dx12Backend::clear_owned_frame_submission_state() noexcept {
  if (impl_) {
    clear_owned_frame_state(*impl_);
  }
}

Status Dx12Backend::rebind_host(const Dx12HostBinding& binding) {
  if (config_.host.host_mode == HostMode::owned_window) {
    return Status::invalid_argument("Dx12Backend rebind_host is only valid for external or injected host modes.");
  }
  if (binding.device == nullptr || binding.command_queue == nullptr || binding.swap_chain == nullptr) {
    return Status::invalid_argument("Dx12Backend host rebinding requires a device, command queue, and swap chain.");
  }

  const ID3D12Device* previous_device = impl_->device;
  config_.window_handle = binding.window_handle;
  config_.device = binding.device;
  config_.command_queue = binding.command_queue;
  config_.command_list = binding.command_list;
  config_.swap_chain = binding.swap_chain;
  if (binding.viewport.width > 0 && binding.viewport.height > 0) {
    viewport_ = binding.viewport;
  }

  if (!initialized_) {
    return initialize();
  }

  const bool device_changed = previous_device != binding.device;
  reset_device_objects(device_changed);
  clear_frame_binding();
  clear_owned_frame_submission_state();
  impl_->device = binding.device;
  impl_->queue = binding.command_queue;
  impl_->swap_chain = binding.swap_chain;
  if (viewport_.width == 0 || viewport_.height == 0) {
    DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
    const HRESULT desc_hr = impl_->swap_chain->GetDesc(&swap_chain_desc);
    if (FAILED(desc_hr)) {
      return Status::backend_error("Dx12Backend failed to query the rebound swap-chain description: " + hex_hr(desc_hr));
    }
    viewport_.width = std::max(swap_chain_desc.BufferDesc.Width, 1u);
    viewport_.height = std::max(swap_chain_desc.BufferDesc.Height, 1u);
  }
  if (device_changed || !impl_->root_signature || !impl_->solid_pso || !impl_->textured_pso) {
    return create_device_objects();
  }
  return Status::success();
}

void Dx12Backend::detach_host() noexcept {
  if (!impl_ || owns_device_objects_) {
    return;
  }
  reset_device_objects(true);
  clear_frame_binding();
  clear_owned_frame_submission_state();
  impl_->device = nullptr;
  impl_->queue = nullptr;
  impl_->swap_chain = nullptr;
  config_.window_handle = nullptr;
  config_.device = nullptr;
  config_.command_queue = nullptr;
  config_.command_list = nullptr;
  config_.swap_chain = nullptr;
  viewport_ = {};
  last_widget_count_ = 0;
  initialized_ = false;
  release_transient_storage();
}

std::size_t Dx12Backend::last_widget_count() const noexcept { return last_widget_count_; }
bool Dx12Backend::initialized() const noexcept { return initialized_; }
bool Dx12Backend::owns_device_objects() const noexcept { return owns_device_objects_; }
bool Dx12Backend::has_frame_binding() const noexcept { return has_frame_binding_; }
bool Dx12Backend::has_font(std::string_view key) const noexcept { return impl_ && impl_->font_descriptors.contains(key); }
bool Dx12Backend::has_image(std::string_view key) const noexcept { return impl_ && impl_->image_resources.contains(key); }
bool Dx12Backend::has_shader(std::string_view key) const noexcept { return impl_ && impl_->shader_descriptors.contains(key); }
bool Dx12Backend::has_texture(std::string_view key) const noexcept { return impl_ && impl_->textures.contains(key); }
HWND Dx12Backend::window_handle() const noexcept { return config_.window_handle; }
ID3D12Device* Dx12Backend::device_handle() const noexcept { return impl_ ? impl_->device : nullptr; }
ID3D12CommandQueue* Dx12Backend::command_queue_handle() const noexcept { return impl_ ? impl_->queue : nullptr; }
ID3D12GraphicsCommandList* Dx12Backend::command_list_handle() const noexcept {
  if (!impl_) {
    return nullptr;
  }
  if (has_frame_binding_) {
    return frame_binding_.command_list;
  }
  return impl_->owned_command_list.Get();
}
IDXGISwapChain3* Dx12Backend::swap_chain_handle() const noexcept { return impl_ ? impl_->swap_chain : nullptr; }

void Dx12Backend::release_transient_storage() noexcept {
  if (!impl_) {
    return;
  }
  impl_->scratch_scene.quads.clear();
  impl_->scratch_scene.labels.clear();
  impl_->scratch_scene.wide_text_cache = nullptr;
  impl_->scratch_text_placements.clear();
  detail::release_storage(impl_->scratch_scene.quads);
  detail::release_storage(impl_->scratch_scene.labels);
  detail::release_storage(impl_->scratch_text_placements);
  detail::release_storage(impl_->scratch_vertices);
  detail::release_storage(impl_->scratch_batches);
  detail::release_storage(impl_->wide_text_cache);
}

}  // namespace igr::backends





