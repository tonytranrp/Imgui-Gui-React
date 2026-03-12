#include <iostream>
#include <string>
#include <string_view>

#include "igr/context.hpp"
#include "igr/react/transport.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

const igr::WidgetAttribute* find_attribute(const igr::WidgetNode& node, std::string_view name) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name == name) {
      return &attribute;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  const std::string payload = R"({
    "kind":"igr.document.v1",
    "sequence":42,
    "session":{
      "name":"react-native-test-physics",
      "targetBackend":"any",
      "host":{
        "hostMode":"injected_overlay",
        "presentationMode":"host_managed",
        "resizeMode":"host_managed",
        "inputMode":"external_forwarded",
        "clearTarget":false,
        "restoreHostState":true
      }
    },
    "fonts":[
      {"key":"body-md","family":"Segoe UI","size":15,"weight":"medium","style":"normal","locale":"en-us"}
    ],
    "images":[
      {"key":"preview-card","texture":"texture-0","width":96,"height":48,"u":0,"v":0,"uvWidth":1,"uvHeight":1,"tint":"#FFFFFFFF"}
    ],
    "shaders":[
      {
        "key":"warp-grid",
        "pixel":{"language":"glsl","source":"vec4 mainImage(vec2 uv) { return vec4(uv, 0.5, 1.0); }"},
        "samplesTexture":false,
        "blendMode":"additive"
      },
      {
        "key":"preview-composite",
        "vertex":{"language":"hlsl","entryPoint":"vsMain","source":"float4 vsMain(float4 position : POSITION) : SV_Position { return position; }"},
        "pixel":{"language":"hlsl","entryPoint":"psMain","source":"float4 psMain(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target { return float4(uv, 0.25, 1.0); }"},
        "samplesTexture":true,
        "blendMode":"alpha"
      }
    ],
    "root":{
      "type":"window",
      "key":"transport-root",
      "props":{"title":"Transport Root","x":40,"y":24,"width":320,"height":180},
      "children":[
        {
          "type":"stack",
          "key":"column",
          "props":{"axis":"vertical"},
          "children":[
            {"type":"text","key":"headline","props":{"value":"Native transport","font":"body-md"},"children":[]},
            {
              "type":"shader_rect",
              "key":"shader-preview",
              "props":{
                "shader":"preview-composite",
                "x":8,
                "y":6,
                "width":96,
                "height":48,
                "resource":"preview-card",
                "texture":"texture-0",
                "tint":"#88CCFFFF",
                "param0":"1.0, 0.5, 0.25, 1.0",
                "param1":"0.0, 1.0, 0.0, 0.0"
              },
              "children":[]
            },
            {"type":"image","key":"preview","props":{"texture":"texture-0","resource":"preview-card","width":96,"height":48,"label":"Preview"},"children":[]}
          ]
        }
      ]
    }
  })";

  igr::react::TransportEnvelope envelope;
  igr::Status status = igr::react::parse_transport_envelope(payload, &envelope);
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("parse_transport_envelope failed");
  }

  if (envelope.sequence != 42 || envelope.root.type != "window" || envelope.root.children.size() != 1 || envelope.fonts.size() != 1 ||
      envelope.images.size() != 1 || envelope.shaders.size() != 2 || envelope.session.host.host_mode != igr::HostMode::injected_overlay) {
    return fail("parse_transport_envelope did not preserve the transport structure");
  }

  if (envelope.shaders[0].descriptor.pixel.language != igr::ShaderLanguage::glsl ||
      envelope.shaders[0].descriptor.blend_mode != igr::ShaderBlendMode::additive ||
      !envelope.shaders[1].descriptor.samples_texture ||
      envelope.shaders[1].descriptor.vertex.entry_point != "vsMain") {
    return fail("parse_transport_envelope did not preserve shader resource metadata");
  }

  std::string serialized;
  status = igr::react::serialize_transport_envelope(envelope, &serialized);
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("serialize_transport_envelope failed");
  }

  if (serialized.find("preview-composite") == std::string::npos || serialized.find("blendMode") == std::string::npos ||
      serialized.find("react-native-test-physics") == std::string::npos) {
    return fail("serialize_transport_envelope dropped shader or session metadata");
  }

  igr::UiContext context;
  status = context.begin_frame({
      .frame_index = 42,
      .viewport = {640, 360},
      .delta_seconds = 1.0 / 60.0,
  });
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("begin_frame failed");
  }

  status = igr::react::materialize_transport_envelope(envelope, context.builder());
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("materialize_transport_envelope failed");
  }

  const igr::FrameDocument document = context.end_frame();
  if (document.widget_count() != 5) {
    return fail("transport materialization produced an unexpected widget count");
  }

  const auto& stack = document.roots.front().children.front();
  if (stack.children[0].attributes.front().value != "body-md") {
    return fail("transport materialization did not preserve the text font key");
  }

  const auto& shader_rect = stack.children[1];
  const auto* primitive = find_attribute(shader_rect, "primitive");
  const auto* shader = find_attribute(shader_rect, "shader");
  const auto* resource = find_attribute(shader_rect, "resource");
  if (shader_rect.kind != igr::WidgetKind::custom_draw || primitive == nullptr || primitive->value != "shader_rect" || shader == nullptr ||
      shader->value != "preview-composite" || resource == nullptr || resource->value != "preview-card") {
    return fail("transport materialization did not preserve shader_rect metadata");
  }

  if (const auto* param0x = find_attribute(shader_rect, "param0_r"); param0x == nullptr || param0x->value != "1") {
    return fail("transport materialization did not parse shader_rect param0");
  }

  if (const auto* image_resource = find_attribute(stack.children[2], "resource");
      image_resource == nullptr || image_resource->value != "preview-card") {
    return fail("transport materialization did not preserve the image resource key");
  }

  std::cout << "igr_react_transport_tests passed" << '\n';
  return 0;
}
