#include <iostream>

#include "igr/shader_compiler.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  {
    igr::ShaderResourceDesc shader{};
    shader.pixel.language = igr::ShaderLanguage::hlsl;
    shader.pixel.entry_point = "main";
    shader.pixel.source = R"(
struct PSInput {
  float4 position : SV_POSITION;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
  return float4(input.uv.x, input.uv.y, 0.5, 1.0) * input.color;
}
)";

    igr::shaders::CompiledProgram program;
    igr::Status status = igr::shaders::compile_program(shader, {.debug_info = false}, &program);
    if (!status) {
      std::cerr << status.message() << '\n';
      return fail("HLSL shader compilation failed");
    }
    if (program.has_custom_vertex || program.pixel.bytecode.empty() || program.pixel.hlsl_source.empty()) {
      return fail("HLSL shader compilation produced an unexpected program layout");
    }
  }

  {
    igr::ShaderResourceDesc shader{};
    shader.pixel.language = igr::ShaderLanguage::glsl;
    shader.pixel.entry_point = "main";
    shader.pixel.source = R"(
#version 450
layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inUv;
layout(location = 0) out vec4 outColor;

void main() {
  outColor = vec4(inUv.x, inUv.y, 0.5, 1.0) * inColor;
}
)";

    igr::shaders::CompiledProgram program;
    igr::Status status = igr::shaders::compile_program(shader, {.debug_info = false}, &program);
    if (!status) {
      std::cerr << status.message() << '\n';
      return fail("GLSL shader compilation failed");
    }
    if (program.has_custom_vertex || program.pixel.bytecode.empty() || program.pixel.hlsl_source.empty()) {
      return fail("GLSL shader compilation produced an unexpected program layout");
    }
  }

  std::cout << "igr_shader_compiler_tests passed" << '\n';
  return 0;
}
