#include "igr/shader_compiler.hpp"

#include <cstring>
#include <exception>
#include <mutex>
#include <sstream>
#include <string_view>
#include <vector>

#include <d3dcompiler.h>
#if IGR_ENABLE_GLSL_SHADERS
#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>
#include <spirv_hlsl.hpp>
#endif
#include <wrl/client.h>

namespace igr::shaders {
namespace {

using Microsoft::WRL::ComPtr;

std::string shader_stage_name(ProgramStage stage) {
  return stage == ProgramStage::vertex ? "vertex" : "pixel";
}

std::string hex_hr(HRESULT hr) {
  std::ostringstream stream;
  stream << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
  return stream.str();
}

const char* to_profile(ProgramStage stage) noexcept {
  return stage == ProgramStage::vertex ? "vs_5_0" : "ps_5_0";
}

#if IGR_ENABLE_GLSL_SHADERS
struct GlslangProcess {
  GlslangProcess() { glslang_initialize_process(); }
  ~GlslangProcess() { glslang_finalize_process(); }
};

const GlslangProcess& glslang_process() {
  static const GlslangProcess process;
  return process;
}

glslang_stage_t to_glslang_stage(ProgramStage stage) noexcept {
  return stage == ProgramStage::vertex ? GLSLANG_STAGE_VERTEX : GLSLANG_STAGE_FRAGMENT;
}

const char* info_or_empty(const char* value) noexcept {
  return value == nullptr ? "" : value;
}

Status translate_glsl_to_hlsl(const ShaderStageDesc& descriptor, ProgramStage stage, std::string* output) {
  if (output == nullptr) {
    return Status::invalid_argument("translate_glsl_to_hlsl requires a valid output string.");
  }
  if (descriptor.source.empty()) {
    return Status::invalid_argument("GLSL shader source cannot be empty.");
  }

  (void)glslang_process();

  glslang_input_t input{};
  input.language = GLSLANG_SOURCE_GLSL;
  input.stage = to_glslang_stage(stage);
  input.client = GLSLANG_CLIENT_VULKAN;
  input.client_version = GLSLANG_TARGET_VULKAN_1_1;
  input.target_language = GLSLANG_TARGET_SPV;
  input.target_language_version = GLSLANG_TARGET_SPV_1_0;
  input.code = descriptor.source.c_str();
  input.default_version = 450;
  input.default_profile = GLSLANG_NO_PROFILE;
  input.force_default_version_and_profile = 0;
  input.forward_compatible = 0;
  input.messages = static_cast<glslang_messages_t>(0);
  input.resource = glslang_default_resource();

  glslang_shader_t* shader = glslang_shader_create(&input);
  if (shader == nullptr) {
    return Status::backend_error("Failed to create a glslang shader object.");
  }

  const std::string entry_point = descriptor.entry_point.empty() ? std::string("main") : descriptor.entry_point;
  glslang_shader_set_entry_point(shader, entry_point.c_str());

  if (glslang_shader_preprocess(shader, &input) == 0) {
    const std::string message = std::string(info_or_empty(glslang_shader_get_info_log(shader))) +
                                std::string(info_or_empty(glslang_shader_get_info_debug_log(shader)));
    glslang_shader_delete(shader);
    return Status::backend_error("GLSL preprocess failed for the " + shader_stage_name(stage) + " shader: " + message);
  }

  if (glslang_shader_parse(shader, &input) == 0) {
    const std::string message = std::string(info_or_empty(glslang_shader_get_info_log(shader))) +
                                std::string(info_or_empty(glslang_shader_get_info_debug_log(shader)));
    glslang_shader_delete(shader);
    return Status::backend_error("GLSL parse failed for the " + shader_stage_name(stage) + " shader: " + message);
  }

  glslang_program_t* program = glslang_program_create();
  if (program == nullptr) {
    glslang_shader_delete(shader);
    return Status::backend_error("Failed to create a glslang program.");
  }

  glslang_program_add_shader(program, shader);
  if (glslang_program_link(program, static_cast<glslang_messages_t>(0)) == 0) {
    const std::string message = std::string(info_or_empty(glslang_program_get_info_log(program))) +
                                std::string(info_or_empty(glslang_program_get_info_debug_log(program)));
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return Status::backend_error("GLSL link failed for the " + shader_stage_name(stage) + " shader: " + message);
  }

  glslang_spv_options_t spv_options{};
  spv_options.disable_optimizer = true;
  glslang_program_SPIRV_generate_with_options(program, to_glslang_stage(stage), &spv_options);
  const char* spirv_message = glslang_program_SPIRV_get_messages(program);
  if (spirv_message != nullptr && spirv_message[0] != '\0') {
    const std::string message(spirv_message);
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return Status::backend_error("GLSL SPIR-V generation failed for the " + shader_stage_name(stage) + " shader: " + message);
  }

  const size_t word_count = glslang_program_SPIRV_get_size(program);
  const unsigned int* words_ptr = glslang_program_SPIRV_get_ptr(program);
  std::vector<std::uint32_t> words(word_count);
  if (word_count > 0 && words_ptr != nullptr) {
    std::memcpy(words.data(), words_ptr, word_count * sizeof(std::uint32_t));
  }

  glslang_program_delete(program);
  glslang_shader_delete(shader);

  try {
    spirv_cross::CompilerHLSL compiler(words);
    spirv_cross::CompilerHLSL::Options hlsl_options{};
    hlsl_options.shader_model = 50;
    compiler.set_hlsl_options(hlsl_options);
    *output = compiler.compile();
  } catch (const std::exception& ex) {
    return Status::backend_error("SPIRV-Cross failed to translate the " + shader_stage_name(stage) + " shader to HLSL: " + std::string(ex.what()));
  }

  return Status::success();
}
#endif

Status stage_to_hlsl(const ShaderStageDesc& descriptor, ProgramStage stage, std::string* output) {
  if (output == nullptr) {
    return Status::invalid_argument("stage_to_hlsl requires a valid output string.");
  }
  if (descriptor.source.empty()) {
    output->clear();
    return Status::success();
  }
  if (descriptor.language == ShaderLanguage::hlsl) {
    *output = descriptor.source;
    return Status::success();
  }
#if IGR_ENABLE_GLSL_SHADERS
  return translate_glsl_to_hlsl(descriptor, stage, output);
#else
  (void)stage;
  return Status::invalid_argument("GLSL shader support was disabled for this build.");
#endif
}

Status compile_hlsl(const ShaderStageDesc& descriptor,
                    ProgramStage stage,
                    const CompileOptions& options,
                    CompiledStage* compiled_stage) {
  if (compiled_stage == nullptr) {
    return Status::invalid_argument("compile_hlsl requires a valid output stage.");
  }
  if (descriptor.source.empty()) {
    *compiled_stage = {};
    return Status::success();
  }

  std::string hlsl_source;
  Status status = stage_to_hlsl(descriptor, stage, &hlsl_source);
  if (!status) {
    return status;
  }

  UINT flags = 0;
#if defined(_DEBUG)
  if (options.debug_info) {
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
  }
#endif

  const std::string entry_point = descriptor.entry_point.empty() ? std::string("main") : descriptor.entry_point;
  ComPtr<ID3DBlob> shader_blob;
  ComPtr<ID3DBlob> error_blob;
  const HRESULT hr = D3DCompile(
      hlsl_source.data(),
      hlsl_source.size(),
      nullptr,
      nullptr,
      nullptr,
      entry_point.c_str(),
      to_profile(stage),
      flags,
      0,
      &shader_blob,
      &error_blob);
  if (FAILED(hr)) {
    std::string error_message = "Shader compile failed for the " + shader_stage_name(stage) + " stage";
    if (error_blob != nullptr && error_blob->GetBufferSize() > 0) {
      error_message += ": ";
      error_message.append(static_cast<const char*>(error_blob->GetBufferPointer()), error_blob->GetBufferSize());
    } else {
      error_message += ": " + hex_hr(hr);
    }
    return Status::backend_error(error_message);
  }

  compiled_stage->hlsl_source = std::move(hlsl_source);
  compiled_stage->bytecode.resize(shader_blob->GetBufferSize());
  std::memcpy(compiled_stage->bytecode.data(), shader_blob->GetBufferPointer(), shader_blob->GetBufferSize());
  return Status::success();
}

}  // namespace

Status compile_program(const ShaderResourceDesc& descriptor, const CompileOptions& options, CompiledProgram* program) {
  if (program == nullptr) {
    return Status::invalid_argument("compile_program requires a valid output program.");
  }
  if (descriptor.pixel.source.empty()) {
    return Status::invalid_argument("ShaderResourceDesc requires a pixel shader source.");
  }

  *program = {};
  program->has_custom_vertex = !descriptor.vertex.source.empty();
  program->samples_texture = descriptor.samples_texture;
  program->blend_mode = descriptor.blend_mode;

  Status status = compile_hlsl(descriptor.pixel, ProgramStage::pixel, options, &program->pixel);
  if (!status) {
    return status;
  }

  if (program->has_custom_vertex) {
    status = compile_hlsl(descriptor.vertex, ProgramStage::vertex, options, &program->vertex);
    if (!status) {
      return status;
    }
  }

  return Status::success();
}

}  // namespace igr::shaders
