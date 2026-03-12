#include "igr/resources.hpp"

namespace igr {

std::string_view to_string(FontWeight weight) noexcept {
  switch (weight) {
    case FontWeight::regular:
      return "regular";
    case FontWeight::medium:
      return "medium";
    case FontWeight::semibold:
      return "semibold";
    case FontWeight::bold:
      return "bold";
  }
  return "regular";
}

std::string_view to_string(FontStyle style) noexcept {
  switch (style) {
    case FontStyle::normal:
      return "normal";
    case FontStyle::italic:
      return "italic";
  }
  return "normal";
}

std::string_view to_string(ShaderLanguage language) noexcept {
  switch (language) {
    case ShaderLanguage::hlsl:
      return "hlsl";
    case ShaderLanguage::glsl:
      return "glsl";
  }
  return "hlsl";
}

std::string_view to_string(ShaderBlendMode blend_mode) noexcept {
  switch (blend_mode) {
    case ShaderBlendMode::alpha:
      return "alpha";
    case ShaderBlendMode::opaque:
      return "opaque";
    case ShaderBlendMode::additive:
      return "additive";
  }
  return "alpha";
}

}  // namespace igr
