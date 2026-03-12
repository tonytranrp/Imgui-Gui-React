#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "igr/result.hpp"
#include "igr/shader.hpp"

namespace igr::shaders {

enum class ProgramStage {
  vertex,
  pixel,
};

struct CompiledStage {
  std::string hlsl_source;
  std::vector<std::uint8_t> bytecode;
};

struct CompiledProgram {
  bool has_custom_vertex{};
  bool samples_texture{};
  ShaderBlendMode blend_mode{ShaderBlendMode::alpha};
  CompiledStage vertex;
  CompiledStage pixel;
};

struct CompileOptions {
  bool debug_info{false};
};

Status compile_program(const ShaderResourceDesc& descriptor, const CompileOptions& options, CompiledProgram* program);

}  // namespace igr::shaders
