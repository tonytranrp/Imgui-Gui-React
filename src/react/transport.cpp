#include "igr/react/transport.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <sstream>
#include <utility>

namespace igr::react {
namespace {

std::string_view to_string(HostMode mode) noexcept {
  switch (mode) {
    case HostMode::owned_window:
      return "owned_window";
    case HostMode::external_swap_chain:
      return "external_swap_chain";
    case HostMode::injected_overlay:
      return "injected_overlay";
  }
  return "external_swap_chain";
}

std::string_view to_string(PresentationMode mode) noexcept {
  switch (mode) {
    case PresentationMode::backend_managed:
      return "backend_managed";
    case PresentationMode::host_managed:
      return "host_managed";
  }
  return "backend_managed";
}

std::string_view to_string(ResizeMode mode) noexcept {
  switch (mode) {
    case ResizeMode::backend_managed:
      return "backend_managed";
    case ResizeMode::host_managed:
      return "host_managed";
  }
  return "backend_managed";
}

std::string_view to_string(InputMode mode) noexcept {
  switch (mode) {
    case InputMode::none:
      return "none";
    case InputMode::external_forwarded:
      return "external_forwarded";
    case InputMode::subclassed_window_proc:
      return "subclassed_window_proc";
  }
  return "external_forwarded";
}

bool parse_host_mode(std::string_view value, HostMode* mode) {
  if (value == "owned_window") {
    *mode = HostMode::owned_window;
    return true;
  }
  if (value == "external_swap_chain") {
    *mode = HostMode::external_swap_chain;
    return true;
  }
  if (value == "injected_overlay") {
    *mode = HostMode::injected_overlay;
    return true;
  }
  return false;
}

bool parse_presentation_mode(std::string_view value, PresentationMode* mode) {
  if (value == "backend_managed") {
    *mode = PresentationMode::backend_managed;
    return true;
  }
  if (value == "host_managed") {
    *mode = PresentationMode::host_managed;
    return true;
  }
  return false;
}

bool parse_resize_mode(std::string_view value, ResizeMode* mode) {
  if (value == "backend_managed") {
    *mode = ResizeMode::backend_managed;
    return true;
  }
  if (value == "host_managed") {
    *mode = ResizeMode::host_managed;
    return true;
  }
  return false;
}

bool parse_input_mode(std::string_view value, InputMode* mode) {
  if (value == "none") {
    *mode = InputMode::none;
    return true;
  }
  if (value == "external_forwarded") {
    *mode = InputMode::external_forwarded;
    return true;
  }
  if (value == "subclassed_window_proc") {
    *mode = InputMode::subclassed_window_proc;
    return true;
  }
  return false;
}

bool parse_font_weight(std::string_view value, FontWeight* weight) {
  if (value == "regular") {
    *weight = FontWeight::regular;
    return true;
  }
  if (value == "medium") {
    *weight = FontWeight::medium;
    return true;
  }
  if (value == "semibold") {
    *weight = FontWeight::semibold;
    return true;
  }
  if (value == "bold") {
    *weight = FontWeight::bold;
    return true;
  }
  return false;
}

bool parse_font_style(std::string_view value, FontStyle* style) {
  if (value == "normal") {
    *style = FontStyle::normal;
    return true;
  }
  if (value == "italic") {
    *style = FontStyle::italic;
    return true;
  }
  return false;
}

bool parse_shader_language(std::string_view value, ShaderLanguage* language) {
  if (value == "hlsl") {
    *language = ShaderLanguage::hlsl;
    return true;
  }
  if (value == "glsl") {
    *language = ShaderLanguage::glsl;
    return true;
  }
  return false;
}

bool parse_shader_blend_mode(std::string_view value, ShaderBlendMode* blend_mode) {
  if (value == "alpha") {
    *blend_mode = ShaderBlendMode::alpha;
    return true;
  }
  if (value == "opaque") {
    *blend_mode = ShaderBlendMode::opaque;
    return true;
  }
  if (value == "additive") {
    *blend_mode = ShaderBlendMode::additive;
    return true;
  }
  return false;
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

std::optional<ColorRgba> parse_color_rgba(std::string_view value) {
  if (value.size() != 7 && value.size() != 9) {
    return std::nullopt;
  }
  if (value.front() != '#') {
    return std::nullopt;
  }

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
  const auto a = value.size() == 9 ? decode(7) : std::optional<float>(1.0f);
  if (!r.has_value() || !g.has_value() || !b.has_value() || !a.has_value()) {
    return std::nullopt;
  }
  return ColorRgba{*r, *g, *b, *a};
}

std::string serialize_color_rgba(const ColorRgba& color) {
  auto channel = [](float value) {
    return static_cast<unsigned int>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
  };

  std::ostringstream stream;
  stream << '#';
  stream << std::hex << std::uppercase;
  stream.width(2);
  stream.fill('0');
  stream << channel(color[0]);
  stream.width(2);
  stream << channel(color[1]);
  stream.width(2);
  stream << channel(color[2]);
  stream.width(2);
  stream << channel(color[3]);
  return stream.str();
}

class JsonParser {
 public:
  explicit JsonParser(std::string_view input) : input_(input) {}

  Status parse(TransportEnvelope* envelope) {
    if (envelope == nullptr) {
      return Status::invalid_argument("parse_transport_envelope requires a valid output envelope.");
    }
    *envelope = {};

    skip_ws();
    Status status = expect('{', "Transport payload must start with an object.");
    if (!status) {
      return status;
    }

    bool has_kind = false;
    bool has_root = false;
    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Transport object fields require ':'.");
      if (!status) {
        return status;
      }

      if (field_name == "kind") {
        status = parse_string(&envelope->kind);
        if (!status) {
          return status;
        }
        has_kind = true;
      } else if (field_name == "sequence") {
        PropertyValue value;
        status = parse_property_value(&value);
        if (!status) {
          return status;
        }
        if (const auto* integer_value = std::get_if<std::int64_t>(&value)) {
          envelope->sequence = static_cast<std::uint64_t>(std::max<std::int64_t>(*integer_value, 0));
        } else if (const auto* double_value = std::get_if<double>(&value)) {
          envelope->sequence = static_cast<std::uint64_t>(std::max(*double_value, 0.0));
        } else {
          return Status::invalid_argument("Transport sequence must be numeric.");
        }
      } else if (field_name == "root") {
        status = parse_element(&envelope->root);
        if (!status) {
          return status;
        }
        has_root = true;
      } else if (field_name == "session") {
        status = parse_session(&envelope->session);
        if (!status) {
          return status;
        }
      } else if (field_name == "fonts") {
        status = parse_fonts(&envelope->fonts);
        if (!status) {
          return status;
        }
      } else if (field_name == "images") {
        status = parse_images(&envelope->images);
        if (!status) {
          return status;
        }
      } else if (field_name == "shaders") {
        status = parse_shaders(&envelope->shaders);
        if (!status) {
          return status;
        }
      } else {
        status = skip_value();
        if (!status) {
          return status;
        }
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Transport object fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }

    skip_ws();
    if (!eof()) {
      return Status::invalid_argument("Transport payload contains unexpected trailing characters.");
    }
    if (!has_kind || envelope->kind.empty()) {
      return Status::invalid_argument("Transport payload is missing the document kind.");
    }
    if (!has_root) {
      return Status::invalid_argument("Transport payload is missing the root element.");
    }
    return Status::success();
  }

 private:
  Status parse_element(ElementNode* element) {
    if (element == nullptr) {
      return Status::invalid_argument("Element output is null.");
    }

    *element = {};
    Status status = expect('{', "Element payload must be an object.");
    if (!status) {
      return status;
    }

    bool has_type = false;
    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Element fields require ':'.");
      if (!status) {
        return status;
      }

      if (field_name == "type") {
        status = parse_string(&element->type);
        if (!status) {
          return status;
        }
        has_type = true;
      } else if (field_name == "key") {
        if (peek() == 'n') {
          status = parse_null();
          if (!status) {
            return status;
          }
          element->key.clear();
        } else {
          status = parse_string(&element->key);
          if (!status) {
            return status;
          }
        }
      } else if (field_name == "props") {
        status = parse_props(&element->props);
        if (!status) {
          return status;
        }
      } else if (field_name == "children") {
        status = parse_children(&element->children);
        if (!status) {
          return status;
        }
      } else {
        status = skip_value();
        if (!status) {
          return status;
        }
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Element fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }

    if (!has_type || element->type.empty()) {
      return Status::invalid_argument("Element payload is missing the type field.");
    }
    return Status::success();
  }

  Status parse_props(std::vector<Property>* props) {
    if (props == nullptr) {
      return Status::invalid_argument("Property output is null.");
    }
    props->clear();
    Status status = expect('{', "props must be an object.");
    if (!status) {
      return status;
    }
    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      Property property;
      status = parse_string(&property.name);
      if (!status) {
        return status;
      }
      status = expect(':', "Property fields require ':'.");
      if (!status) {
        return status;
      }
      status = parse_property_value(&property.value);
      if (!status) {
        return status;
      }
      props->push_back(std::move(property));

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Property fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }
    return Status::success();
  }

  Status parse_children(std::vector<ElementNode>* children) {
    if (children == nullptr) {
      return Status::invalid_argument("Children output is null.");
    }
    children->clear();
    Status status = expect('[', "children must be an array.");
    if (!status) {
      return status;
    }
    while (true) {
      skip_ws();
      if (consume(']')) {
        break;
      }
      ElementNode child;
      status = parse_element(&child);
      if (!status) {
        return status;
      }
      children->push_back(std::move(child));
      skip_ws();
      if (consume(']')) {
        break;
      }
      status = expect(',', "Children elements must be comma-separated.");
      if (!status) {
        return status;
      }
    }
    return Status::success();
  }

  Status parse_session(TransportSession* session) {
    if (session == nullptr) {
      return Status::invalid_argument("Session output is null.");
    }

    *session = {};
    Status status = expect('{', "session must be an object.");
    if (!status) {
      return status;
    }

    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Session fields require ':'.");
      if (!status) {
        return status;
      }

      if (field_name == "name") {
        status = parse_string(&session->name);
      } else if (field_name == "targetBackend") {
        status = parse_string(&session->target_backend);
      } else if (field_name == "host") {
        status = parse_host_options(&session->host);
      } else {
        status = skip_value();
      }
      if (!status) {
        return status;
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Session fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }

    if (session->target_backend.empty()) {
      session->target_backend = "any";
    }
    return Status::success();
  }

  Status parse_host_options(BackendHostOptions* host) {
    if (host == nullptr) {
      return Status::invalid_argument("Host options output is null.");
    }

    *host = {};
    Status status = expect('{', "host must be an object.");
    if (!status) {
      return status;
    }

    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Host fields require ':'.");
      if (!status) {
        return status;
      }

      if (field_name == "hostMode") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_host_mode(value, &host->host_mode)) {
          return Status::invalid_argument("Unsupported hostMode value in transport payload.");
        }
      } else if (field_name == "presentationMode") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_presentation_mode(value, &host->presentation_mode)) {
          return Status::invalid_argument("Unsupported presentationMode value in transport payload.");
        }
      } else if (field_name == "resizeMode") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_resize_mode(value, &host->resize_mode)) {
          return Status::invalid_argument("Unsupported resizeMode value in transport payload.");
        }
      } else if (field_name == "inputMode") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_input_mode(value, &host->input_mode)) {
          return Status::invalid_argument("Unsupported inputMode value in transport payload.");
        }
      } else if (field_name == "clearTarget") {
        PropertyValue value;
        status = parse_property_value(&value);
        if (status) {
          if (const auto* bool_value = std::get_if<bool>(&value)) {
            host->clear_target = *bool_value;
          } else {
            return Status::invalid_argument("Transport host clearTarget must be boolean.");
          }
        }
      } else if (field_name == "restoreHostState") {
        PropertyValue value;
        status = parse_property_value(&value);
        if (status) {
          if (const auto* bool_value = std::get_if<bool>(&value)) {
            host->restore_host_state = *bool_value;
          } else {
            return Status::invalid_argument("Transport host restoreHostState must be boolean.");
          }
        }
      } else {
        status = skip_value();
      }
      if (!status) {
        return status;
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Host fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }
    return Status::success();
  }

  Status parse_fonts(std::vector<TransportFontResource>* fonts) {
    if (fonts == nullptr) {
      return Status::invalid_argument("Font output is null.");
    }
    fonts->clear();
    Status status = expect('[', "fonts must be an array.");
    if (!status) {
      return status;
    }

    while (true) {
      skip_ws();
      if (consume(']')) {
        break;
      }
      TransportFontResource font;
      status = parse_font_resource(&font);
      if (!status) {
        return status;
      }
      fonts->push_back(std::move(font));
      skip_ws();
      if (consume(']')) {
        break;
      }
      status = expect(',', "Font resources must be comma-separated.");
      if (!status) {
        return status;
      }
    }
    return Status::success();
  }

  Status parse_font_resource(TransportFontResource* font) {
    if (font == nullptr) {
      return Status::invalid_argument("Font resource output is null.");
    }
    *font = {};
    Status status = expect('{', "Font resources must be objects.");
    if (!status) {
      return status;
    }

    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Font resource fields require ':'.");
      if (!status) {
        return status;
      }

      if (field_name == "key") {
        status = parse_string(&font->key);
      } else if (field_name == "family") {
        status = parse_string(&font->descriptor.family);
      } else if (field_name == "size") {
        PropertyValue value;
        status = parse_property_value(&value);
        if (status) {
          if (const auto* integer_value = std::get_if<std::int64_t>(&value)) {
            font->descriptor.size = static_cast<float>(*integer_value);
          } else if (const auto* double_value = std::get_if<double>(&value)) {
            font->descriptor.size = static_cast<float>(*double_value);
          } else {
            return Status::invalid_argument("Transport font size must be numeric.");
          }
        }
      } else if (field_name == "weight") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_font_weight(value, &font->descriptor.weight)) {
          return Status::invalid_argument("Unsupported font weight in transport payload.");
        }
      } else if (field_name == "style") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_font_style(value, &font->descriptor.style)) {
          return Status::invalid_argument("Unsupported font style in transport payload.");
        }
      } else if (field_name == "locale") {
        status = parse_string(&font->descriptor.locale);
      } else {
        status = skip_value();
      }
      if (!status) {
        return status;
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Font resource fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }

    if (font->key.empty()) {
      return Status::invalid_argument("Transport font resources require a key.");
    }
    return Status::success();
  }

  Status parse_images(std::vector<TransportImageResource>* images) {
    if (images == nullptr) {
      return Status::invalid_argument("Image output is null.");
    }
    images->clear();
    Status status = expect('[', "images must be an array.");
    if (!status) {
      return status;
    }

    while (true) {
      skip_ws();
      if (consume(']')) {
        break;
      }
      TransportImageResource image;
      status = parse_image_resource(&image);
      if (!status) {
        return status;
      }
      images->push_back(std::move(image));
      skip_ws();
      if (consume(']')) {
        break;
      }
      status = expect(',', "Image resources must be comma-separated.");
      if (!status) {
        return status;
      }
    }
    return Status::success();
  }

  Status parse_image_resource(TransportImageResource* image) {
    if (image == nullptr) {
      return Status::invalid_argument("Image resource output is null.");
    }
    *image = {};
    Status status = expect('{', "Image resources must be objects.");
    if (!status) {
      return status;
    }

    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Image resource fields require ':'.");
      if (!status) {
        return status;
      }

      PropertyValue value;
      if (field_name == "key" || field_name == "texture" || field_name == "tint") {
        std::string text;
        status = parse_string(&text);
        if (!status) {
          return status;
        }
        if (field_name == "key") {
          image->key = std::move(text);
        } else if (field_name == "texture") {
          image->descriptor.texture_key = std::move(text);
        } else {
          const auto tint = parse_color_rgba(text);
          if (!tint.has_value()) {
            return Status::invalid_argument("Image tint must be a #RRGGBB or #RRGGBBAA string.");
          }
          image->descriptor.tint = *tint;
        }
      } else if (field_name == "width" || field_name == "height" || field_name == "u" || field_name == "v" || field_name == "uvWidth" ||
                 field_name == "uvHeight") {
        status = parse_property_value(&value);
        if (!status) {
          return status;
        }
        float numeric = 0.0f;
        if (const auto* integer_value = std::get_if<std::int64_t>(&value)) {
          numeric = static_cast<float>(*integer_value);
        } else if (const auto* double_value = std::get_if<double>(&value)) {
          numeric = static_cast<float>(*double_value);
        } else {
          return Status::invalid_argument("Transport image numeric fields must be numeric.");
        }

        if (field_name == "width") {
          image->descriptor.size.x = numeric;
        } else if (field_name == "height") {
          image->descriptor.size.y = numeric;
        } else if (field_name == "u") {
          image->descriptor.uv.origin.x = numeric;
        } else if (field_name == "v") {
          image->descriptor.uv.origin.y = numeric;
        } else if (field_name == "uvWidth") {
          image->descriptor.uv.extent.x = numeric;
        } else if (field_name == "uvHeight") {
          image->descriptor.uv.extent.y = numeric;
        }
      } else {
        status = skip_value();
      }
      if (!status) {
        return status;
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Image resource fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }

    if (image->key.empty()) {
      return Status::invalid_argument("Transport image resources require a key.");
    }
    if (image->descriptor.texture_key.empty()) {
      return Status::invalid_argument("Transport image resources require a texture key.");
    }
    return Status::success();
  }

  Status parse_shaders(std::vector<TransportShaderResource>* shaders) {
    if (shaders == nullptr) {
      return Status::invalid_argument("Shader output is null.");
    }
    shaders->clear();
    Status status = expect('[', "shaders must be an array.");
    if (!status) {
      return status;
    }

    while (true) {
      skip_ws();
      if (consume(']')) {
        break;
      }
      TransportShaderResource shader;
      status = parse_shader_resource(&shader);
      if (!status) {
        return status;
      }
      shaders->push_back(std::move(shader));
      skip_ws();
      if (consume(']')) {
        break;
      }
      status = expect(',', "Shader resources must be comma-separated.");
      if (!status) {
        return status;
      }
    }
    return Status::success();
  }

  Status parse_shader_resource(TransportShaderResource* shader) {
    if (shader == nullptr) {
      return Status::invalid_argument("Shader resource output is null.");
    }
    *shader = {};
    Status status = expect('{', "Shader resources must be objects.");
    if (!status) {
      return status;
    }

    bool has_pixel = false;
    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Shader resource fields require ':'.");
      if (!status) {
        return status;
      }

      if (field_name == "key") {
        status = parse_string(&shader->key);
      } else if (field_name == "vertex") {
        if (peek() == 'n') {
          status = parse_null();
        } else {
          status = parse_shader_stage(&shader->descriptor.vertex);
        }
      } else if (field_name == "pixel") {
        status = parse_shader_stage(&shader->descriptor.pixel);
        has_pixel = status.ok();
      } else if (field_name == "samplesTexture") {
        PropertyValue value;
        status = parse_property_value(&value);
        if (status) {
          if (const auto* bool_value = std::get_if<bool>(&value)) {
            shader->descriptor.samples_texture = *bool_value;
          } else {
            return Status::invalid_argument("Transport shader samplesTexture must be boolean.");
          }
        }
      } else if (field_name == "blendMode") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_shader_blend_mode(value, &shader->descriptor.blend_mode)) {
          return Status::invalid_argument("Unsupported shader blendMode in transport payload.");
        }
      } else {
        status = skip_value();
      }
      if (!status) {
        return status;
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Shader resource fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }

    if (shader->key.empty()) {
      return Status::invalid_argument("Transport shader resources require a key.");
    }
    if (!has_pixel || shader->descriptor.pixel.source.empty()) {
      return Status::invalid_argument("Transport shader resources require a pixel stage.");
    }
    return Status::success();
  }

  Status parse_shader_stage(ShaderStageDesc* stage) {
    if (stage == nullptr) {
      return Status::invalid_argument("Shader stage output is null.");
    }
    *stage = {};
    Status status = expect('{', "Shader stages must be objects.");
    if (!status) {
      return status;
    }

    bool has_language = false;
    bool has_source = false;
    while (true) {
      skip_ws();
      if (consume('}')) {
        break;
      }

      std::string field_name;
      status = parse_string(&field_name);
      if (!status) {
        return status;
      }
      status = expect(':', "Shader stage fields require ':'.");
      if (!status) {
        return status;
      }

      if (field_name == "language") {
        std::string value;
        status = parse_string(&value);
        if (status && !parse_shader_language(value, &stage->language)) {
          return Status::invalid_argument("Unsupported shader language in transport payload.");
        }
        has_language = status.ok();
      } else if (field_name == "entryPoint") {
        status = parse_string(&stage->entry_point);
      } else if (field_name == "source") {
        status = parse_string(&stage->source);
        has_source = status.ok();
      } else {
        status = skip_value();
      }
      if (!status) {
        return status;
      }

      skip_ws();
      if (consume('}')) {
        break;
      }
      status = expect(',', "Shader stage fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }

    if (!has_language) {
      return Status::invalid_argument("Transport shader stages require a language.");
    }
    if (!has_source || stage->source.empty()) {
      return Status::invalid_argument("Transport shader stages require source text.");
    }
    return Status::success();
  }

  Status parse_property_value(PropertyValue* value) {
    if (value == nullptr) {
      return Status::invalid_argument("Property value output is null.");
    }

    skip_ws();
    const char token = peek();
    if (token == '"') {
      std::string string_value;
      Status status = parse_string(&string_value);
      if (!status) {
        return status;
      }
      *value = std::move(string_value);
      return Status::success();
    }
    if (token == 't' || token == 'f') {
      bool bool_value = false;
      Status status = parse_bool(&bool_value);
      if (!status) {
        return status;
      }
      *value = bool_value;
      return Status::success();
    }
    if (token == 'n') {
      Status status = parse_null();
      if (!status) {
        return status;
      }
      *value = std::monostate{};
      return Status::success();
    }
    if (token == '-' || std::isdigit(static_cast<unsigned char>(token)) != 0) {
      return parse_number(value);
    }

    return Status::invalid_argument("Unsupported property value in transport payload.");
  }

  Status parse_number(PropertyValue* value) {
    const std::size_t start = position_;
    if (peek() == '-') {
      ++position_;
    }
    while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
      ++position_;
    }
    bool is_floating = false;
    if (peek() == '.') {
      is_floating = true;
      ++position_;
      while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        ++position_;
      }
    }
    if (peek() == 'e' || peek() == 'E') {
      is_floating = true;
      ++position_;
      if (peek() == '+' || peek() == '-') {
        ++position_;
      }
      while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        ++position_;
      }
    }

    const std::string_view token = input_.substr(start, position_ - start);
    if (token.empty()) {
      return Status::invalid_argument("Invalid numeric token in transport payload.");
    }

    if (!is_floating) {
      std::int64_t integer_value = 0;
      const auto result = std::from_chars(token.data(), token.data() + token.size(), integer_value);
      if (result.ec == std::errc()) {
        *value = integer_value;
        return Status::success();
      }
    }

    double floating_value = 0.0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), floating_value);
    if (result.ec != std::errc()) {
      return Status::invalid_argument("Invalid numeric token in transport payload.");
    }
    *value = floating_value;
    return Status::success();
  }

  Status parse_bool(bool* value) {
    if (value == nullptr) {
      return Status::invalid_argument("Boolean output is null.");
    }
    if (match("true")) {
      *value = true;
      return Status::success();
    }
    if (match("false")) {
      *value = false;
      return Status::success();
    }
    return Status::invalid_argument("Invalid boolean token in transport payload.");
  }

  Status parse_null() {
    if (match("null")) {
      return Status::success();
    }
    return Status::invalid_argument("Invalid null token in transport payload.");
  }

  Status parse_string(std::string* value) {
    if (value == nullptr) {
      return Status::invalid_argument("String output is null.");
    }
    Status status = expect('"', "Expected a JSON string.");
    if (!status) {
      return status;
    }
    value->clear();
    while (!eof()) {
      const char current = input_[position_++];
      if (current == '"') {
        return Status::success();
      }
      if (current != '\\') {
        value->push_back(current);
        continue;
      }
      if (eof()) {
        return Status::invalid_argument("Unterminated string escape in transport payload.");
      }
      const char escaped = input_[position_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          value->push_back(escaped);
          break;
        case 'b':
          value->push_back('\b');
          break;
        case 'f':
          value->push_back('\f');
          break;
        case 'n':
          value->push_back('\n');
          break;
        case 'r':
          value->push_back('\r');
          break;
        case 't':
          value->push_back('\t');
          break;
        default:
          return Status::invalid_argument("Unsupported string escape in transport payload.");
      }
    }
    return Status::invalid_argument("Unterminated string in transport payload.");
  }

  Status skip_value() {
    skip_ws();
    const char token = peek();
    if (token == '{') {
      return skip_object();
    }
    if (token == '[') {
      return skip_array();
    }
    if (token == '"') {
      std::string ignored;
      return parse_string(&ignored);
    }
    if (token == 't' || token == 'f') {
      bool ignored = false;
      return parse_bool(&ignored);
    }
    if (token == 'n') {
      return parse_null();
    }
    if (token == '-' || std::isdigit(static_cast<unsigned char>(token)) != 0) {
      PropertyValue ignored;
      return parse_number(&ignored);
    }
    return Status::invalid_argument("Unsupported JSON token in transport payload.");
  }

  Status skip_object() {
    Status status = expect('{', "Expected an object.");
    if (!status) {
      return status;
    }
    while (true) {
      skip_ws();
      if (consume('}')) {
        return Status::success();
      }
      std::string ignored;
      status = parse_string(&ignored);
      if (!status) {
        return status;
      }
      status = expect(':', "Object fields require ':'.");
      if (!status) {
        return status;
      }
      status = skip_value();
      if (!status) {
        return status;
      }
      skip_ws();
      if (consume('}')) {
        return Status::success();
      }
      status = expect(',', "Object fields must be comma-separated.");
      if (!status) {
        return status;
      }
    }
  }

  Status skip_array() {
    Status status = expect('[', "Expected an array.");
    if (!status) {
      return status;
    }
    while (true) {
      skip_ws();
      if (consume(']')) {
        return Status::success();
      }
      status = skip_value();
      if (!status) {
        return status;
      }
      skip_ws();
      if (consume(']')) {
        return Status::success();
      }
      status = expect(',', "Array items must be comma-separated.");
      if (!status) {
        return status;
      }
    }
  }

  Status expect(char expected, std::string_view message) {
    skip_ws();
    if (eof() || input_[position_] != expected) {
      return Status::invalid_argument(std::string(message));
    }
    ++position_;
    return Status::success();
  }

  bool consume(char expected) {
    skip_ws();
    if (!eof() && input_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  bool match(std::string_view token) {
    skip_ws();
    if (input_.substr(position_, token.size()) != token) {
      return false;
    }
    position_ += token.size();
    return true;
  }

  void skip_ws() {
    while (!eof() && std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
      ++position_;
    }
  }

  [[nodiscard]] char peek() const noexcept {
    return eof() ? '\0' : input_[position_];
  }

  [[nodiscard]] bool eof() const noexcept {
    return position_ >= input_.size();
  }

  std::string_view input_;
  std::size_t position_{};
};

std::string escape_json(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size() + 8);
  for (const char ch : value) {
    switch (ch) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped.push_back(ch);
        break;
    }
  }
  return escaped;
}

std::string serialize_value(const PropertyValue& value) {
  if (std::holds_alternative<std::monostate>(value)) {
    return "null";
  }
  if (const auto* bool_value = std::get_if<bool>(&value)) {
    return *bool_value ? "true" : "false";
  }
  if (const auto* integer_value = std::get_if<std::int64_t>(&value)) {
    return std::to_string(*integer_value);
  }
  if (const auto* double_value = std::get_if<double>(&value)) {
    std::ostringstream stream;
    stream << *double_value;
    return stream.str();
  }
  return "\"" + escape_json(std::get<std::string>(value)) + "\"";
}

void append_props_json(const std::vector<Property>& props, std::string& output);
void append_element_json(const ElementNode& element, std::string& output);
void append_session_json(const TransportSession& session, std::string& output);
void append_fonts_json(const std::vector<TransportFontResource>& fonts, std::string& output);
void append_images_json(const std::vector<TransportImageResource>& images, std::string& output);
void append_shaders_json(const std::vector<TransportShaderResource>& shaders, std::string& output);

void append_props_json(const std::vector<Property>& props, std::string& output) {
  output += '{';
  for (std::size_t index = 0; index < props.size(); ++index) {
    if (index > 0) {
      output += ',';
    }
    output += '"';
    output += escape_json(props[index].name);
    output += "\":";
    output += serialize_value(props[index].value);
  }
  output += '}';
}

void append_children_json(const std::vector<ElementNode>& children, std::string& output) {
  output += '[';
  for (std::size_t index = 0; index < children.size(); ++index) {
    if (index > 0) {
      output += ',';
    }
    append_element_json(children[index], output);
  }
  output += ']';
}

void append_element_json(const ElementNode& element, std::string& output) {
  output += "{\"type\":\"";
  output += escape_json(element.type);
  output += "\",\"key\":";
  if (element.key.empty()) {
    output += "null";
  } else {
    output += '"';
    output += escape_json(element.key);
    output += '"';
  }
  output += ",\"props\":";
  append_props_json(element.props, output);
  output += ",\"children\":";
  append_children_json(element.children, output);
  output += '}';
}

void append_session_json(const TransportSession& session, std::string& output) {
  output += "{\"name\":\"";
  output += escape_json(session.name);
  output += "\",\"targetBackend\":\"";
  output += escape_json(session.target_backend);
  output += "\",\"host\":{";
  output += "\"hostMode\":\"";
  output += to_string(session.host.host_mode);
  output += "\",\"presentationMode\":\"";
  output += to_string(session.host.presentation_mode);
  output += "\",\"resizeMode\":\"";
  output += to_string(session.host.resize_mode);
  output += "\",\"inputMode\":\"";
  output += to_string(session.host.input_mode);
  output += "\",\"clearTarget\":";
  output += session.host.clear_target ? "true" : "false";
  output += ",\"restoreHostState\":";
  output += session.host.restore_host_state ? "true" : "false";
  output += "}}";
}

void append_fonts_json(const std::vector<TransportFontResource>& fonts, std::string& output) {
  output += '[';
  for (std::size_t index = 0; index < fonts.size(); ++index) {
    if (index > 0) {
      output += ',';
    }
    const auto& font = fonts[index];
    output += "{\"key\":\"";
    output += escape_json(font.key);
    output += "\",\"family\":\"";
    output += escape_json(font.descriptor.family);
    output += "\",\"size\":";
    output += serialize_value(PropertyValue{static_cast<double>(font.descriptor.size)});
    output += ",\"weight\":\"";
    output += igr::to_string(font.descriptor.weight);
    output += "\",\"style\":\"";
    output += igr::to_string(font.descriptor.style);
    output += "\",\"locale\":\"";
    output += escape_json(font.descriptor.locale);
    output += "\"}";
  }
  output += ']';
}

void append_images_json(const std::vector<TransportImageResource>& images, std::string& output) {
  output += '[';
  for (std::size_t index = 0; index < images.size(); ++index) {
    if (index > 0) {
      output += ',';
    }
    const auto& image = images[index];
    output += "{\"key\":\"";
    output += escape_json(image.key);
    output += "\",\"texture\":\"";
    output += escape_json(image.descriptor.texture_key);
    output += "\",\"width\":";
    output += serialize_value(PropertyValue{static_cast<double>(image.descriptor.size.x)});
    output += ",\"height\":";
    output += serialize_value(PropertyValue{static_cast<double>(image.descriptor.size.y)});
    output += ",\"u\":";
    output += serialize_value(PropertyValue{static_cast<double>(image.descriptor.uv.origin.x)});
    output += ",\"v\":";
    output += serialize_value(PropertyValue{static_cast<double>(image.descriptor.uv.origin.y)});
    output += ",\"uvWidth\":";
    output += serialize_value(PropertyValue{static_cast<double>(image.descriptor.uv.extent.x)});
    output += ",\"uvHeight\":";
    output += serialize_value(PropertyValue{static_cast<double>(image.descriptor.uv.extent.y)});
    output += ",\"tint\":\"";
    output += serialize_color_rgba(image.descriptor.tint);
    output += "\"}";
  }
  output += ']';
}

void append_shader_stage_json(const ShaderStageDesc& stage, std::string& output) {
  output += "{\"language\":\"";
  output += igr::to_string(stage.language);
  output += "\",\"entryPoint\":\"";
  output += escape_json(stage.entry_point);
  output += "\",\"source\":\"";
  output += escape_json(stage.source);
  output += "\"}";
}

void append_shaders_json(const std::vector<TransportShaderResource>& shaders, std::string& output) {
  output += '[';
  for (std::size_t index = 0; index < shaders.size(); ++index) {
    if (index > 0) {
      output += ',';
    }
    const auto& shader = shaders[index];
    output += "{\"key\":\"";
    output += escape_json(shader.key);
    output += "\"";
    if (!shader.descriptor.vertex.source.empty()) {
      output += ",\"vertex\":";
      append_shader_stage_json(shader.descriptor.vertex, output);
    }
    output += ",\"pixel\":";
    append_shader_stage_json(shader.descriptor.pixel, output);
    output += ",\"samplesTexture\":";
    output += shader.descriptor.samples_texture ? "true" : "false";
    output += ",\"blendMode\":\"";
    output += igr::to_string(shader.descriptor.blend_mode);
    output += "\"}";
  }
  output += ']';
}

}  // namespace

Status parse_transport_envelope(std::string_view payload, TransportEnvelope* envelope) {
  JsonParser parser(payload);
  return parser.parse(envelope);
}

Status serialize_transport_envelope(const TransportEnvelope& envelope, std::string* payload) {
  if (payload == nullptr) {
    return Status::invalid_argument("serialize_transport_envelope requires a valid output payload.");
  }
  if (envelope.kind.empty()) {
    return Status::invalid_argument("Transport envelope kind cannot be empty.");
  }
  payload->clear();
  payload->reserve(256);
  *payload += "{\"kind\":\"";
  *payload += escape_json(envelope.kind);
  *payload += "\",\"sequence\":";
  *payload += std::to_string(envelope.sequence);
  *payload += ",\"session\":";
  append_session_json(envelope.session, *payload);
  *payload += ",\"fonts\":";
  append_fonts_json(envelope.fonts, *payload);
  *payload += ",\"images\":";
  append_images_json(envelope.images, *payload);
  *payload += ",\"shaders\":";
  append_shaders_json(envelope.shaders, *payload);
  *payload += ",\"root\":";
  append_element_json(envelope.root, *payload);
  *payload += '}';
  return Status::success();
}

Status materialize_transport_envelope(const TransportEnvelope& envelope, FrameBuilder& builder) {
  if (envelope.kind != "igr.document.v1") {
    return Status::invalid_argument("Unsupported transport kind: " + envelope.kind);
  }
  return materialize(envelope.root, builder);
}

Status materialize_transport_envelope(std::string_view payload, FrameBuilder& builder) {
  TransportEnvelope envelope;
  Status status = parse_transport_envelope(payload, &envelope);
  if (!status) {
    return status;
  }
  return materialize_transport_envelope(envelope, builder);
}

}  // namespace igr::react
