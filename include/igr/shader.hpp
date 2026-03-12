#pragma once

#include <array>
#include <string>
#include <string_view>

namespace igr {

using ShaderVector4 = std::array<float, 4>;

enum class ShaderLanguage {
  hlsl,
  glsl,
};

enum class ShaderBlendMode {
  alpha,
  opaque,
  additive,
};

struct ShaderStageDesc {
  ShaderLanguage language{ShaderLanguage::hlsl};
  std::string entry_point{"main"};
  std::string source;
};

struct ShaderUniformData {
  ShaderVector4 tint{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<ShaderVector4, 4> params{
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
      ShaderVector4{0.0f, 0.0f, 0.0f, 0.0f},
  };
};

struct ShaderResourceDesc {
  ShaderStageDesc vertex{};
  ShaderStageDesc pixel{};
  bool samples_texture{false};
  ShaderBlendMode blend_mode{ShaderBlendMode::alpha};
};

[[nodiscard]] std::string_view to_string(ShaderLanguage language) noexcept;
[[nodiscard]] std::string_view to_string(ShaderBlendMode blend_mode) noexcept;

}  // namespace igr
