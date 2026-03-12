#pragma once

#include <string>
#include <string_view>

#include "igr/frame.hpp"
#include "igr/geometry.hpp"
#include "igr/result.hpp"
#include "igr/shader.hpp"

namespace igr {

enum class FontWeight {
  regular,
  medium,
  semibold,
  bold,
};

enum class FontStyle {
  normal,
  italic,
};

[[nodiscard]] std::string_view to_string(FontWeight weight) noexcept;
[[nodiscard]] std::string_view to_string(FontStyle style) noexcept;

struct FontResourceDesc {
  std::string family;
  float size{14.0f};
  FontWeight weight{FontWeight::regular};
  FontStyle style{FontStyle::normal};
  std::string locale;
};

struct ImageResourceDesc {
  std::string texture_key;
  ExtentF size{120.0f, 72.0f};
  Rect uv{{0.0f, 0.0f}, {1.0f, 1.0f}};
  ColorRgba tint{1.0f, 1.0f, 1.0f, 1.0f};
};

class IResourceRegistry {
 public:
  virtual ~IResourceRegistry() = default;

  virtual Status register_font(std::string_view key, const FontResourceDesc& descriptor) = 0;
  virtual void unregister_font(std::string_view key) noexcept = 0;
  virtual Status register_image(std::string_view key, const ImageResourceDesc& descriptor) = 0;
  virtual void unregister_image(std::string_view key) noexcept = 0;
  virtual Status register_shader(std::string_view key, const ShaderResourceDesc& descriptor) = 0;
  virtual void unregister_shader(std::string_view key) noexcept = 0;
};

}  // namespace igr
