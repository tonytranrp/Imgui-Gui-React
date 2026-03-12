#include <Windows.h>



#include <iostream>

#include <wrl/client.h>



#include "igr/backends/dx12_backend.hpp"

#include "igr/context.hpp"



namespace {



using Microsoft::WRL::ComPtr;



struct NullTextureBinding {

  ComPtr<ID3D12DescriptorHeap> heap;

  igr::backends::Dx12TextureBinding binding{};

};

NullTextureBinding create_texture_binding(
    ID3D12Device* device,
    D3D12_DESCRIPTOR_HEAP_TYPE heap_type,
    D3D12_DESCRIPTOR_HEAP_FLAGS heap_flags,
    bool include_cpu_descriptor = true,
    bool include_gpu_descriptor = true) {

  NullTextureBinding texture_binding;

  if (device == nullptr) {

    return texture_binding;

  }

  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{};
  heap_desc.Type = heap_type;
  heap_desc.NumDescriptors = 1;
  heap_desc.Flags = heap_flags;
  if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&texture_binding.heap)))) {
    return {};
  }

  const auto cpu_descriptor = texture_binding.heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor{};
  if (include_gpu_descriptor && (heap_flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0 &&
      heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
    gpu_descriptor = texture_binding.heap->GetGPUDescriptorHandleForHeapStart();
  }

  if (heap_type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(nullptr, &srv_desc, cpu_descriptor);
  }

  texture_binding.binding.heap = texture_binding.heap.Get();
  texture_binding.binding.cpu_descriptor = include_cpu_descriptor ? cpu_descriptor : D3D12_CPU_DESCRIPTOR_HANDLE{};
  texture_binding.binding.gpu_descriptor = include_gpu_descriptor ? gpu_descriptor : D3D12_GPU_DESCRIPTOR_HANDLE{};
  return texture_binding;
}



LRESULT CALLBACK test_window_proc(HWND window_handle, UINT message, WPARAM w_param, LPARAM l_param) {

  return DefWindowProcW(window_handle, message, w_param, l_param);

}



int fail(const char* message) {

  std::cerr << message << '\n';

  return 1;

}



NullTextureBinding create_null_texture_binding(ID3D12Device* device) {
  return create_texture_binding(
      device,
      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE);

}

igr::ShaderResourceDesc make_demo_shader() {
  igr::ShaderResourceDesc shader{};
  shader.pixel.language = igr::ShaderLanguage::hlsl;
  shader.pixel.entry_point = "main";
  shader.pixel.source = R"(
cbuffer IgrShaderConstants : register(b0) {
  float4 igrTint;
  float4 igrParam0;
  float4 igrParam1;
  float4 igrParam2;
  float4 igrParam3;
  float4 igrRect;
  float4 igrViewportAndTime;
  float4 igrFrameData;
};

struct PSInput {
  float4 position : SV_POSITION;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
  const float glow = 0.45 + 0.55 * sin(igrParam0.x * 0.25 + input.uv.x * 6.28318);
  return float4(input.uv.x, input.uv.y, glow, 1.0) * igrTint * input.color;
}
)";
  shader.samples_texture = false;
  shader.blend_mode = igr::ShaderBlendMode::alpha;
  return shader;
}



igr::FrameDocument build_document(std::uint64_t frame_index, igr::ExtentU viewport) {

  igr::UiContext context;

  context.begin_frame({

      .frame_index = frame_index,

      .viewport = viewport,

      .delta_seconds = 1.0 / 60.0,

  });



  auto& builder = context.builder();

  builder.begin_window("dx12-test", "DX12 Test", {{16.0f, 16.0f}, {260.0f, 220.0f}});

  builder.begin_stack("layout", igr::Axis::vertical);

  builder.text("headline", "runtime path", "body-md");

  builder.begin_clip_rect("clip", {180.0f, 124.0f});

  builder.checkbox("checkbox", "DX12 ready", frame_index % 2 == 0);

  builder.progress_bar("progress", "progress", 0.65f);

  builder.image("preview", "texture-0", {96.0f, 48.0f}, "Descriptor-backed image", "preview-card");
  igr::ShaderUniformData shader_uniforms{};
  shader_uniforms.tint = {0.72f, 0.90f, 1.0f, 0.9f};
  shader_uniforms.params[0] = {static_cast<float>(frame_index), 1.0f / 60.0f, static_cast<float>(viewport.width), static_cast<float>(viewport.height)};
  builder.shader_image("shader-preview", "pulse-shader", "texture-0", {96.0f, 48.0f}, "Shader-backed image", "preview-card", shader_uniforms);

  builder.fill_rect("preview-fill", {{8.0f, 8.0f}, {20.0f, 8.0f}}, {0.25f, 0.70f, 0.98f, 0.85f});

  builder.draw_line("preview-line", {6.0f, 102.0f}, {156.0f, 102.0f}, {0.12f, 0.18f, 0.22f, 1.0f}, 2.0f);

  builder.end_container();

  builder.button("button", "commit");

  builder.end_container();

  builder.end_container();



  return context.end_frame();

}

igr::FrameDocument build_text_only_document(std::uint64_t frame_index, igr::ExtentU viewport) {
  igr::UiContext context;
  context.begin_frame({
      .frame_index = frame_index,
      .viewport = viewport,
      .delta_seconds = 1.0 / 60.0,
  });

  auto& builder = context.builder();
  builder.begin_window("dx12-text-only", "DX12 Text Path", {{24.0f, 24.0f}, {280.0f, 160.0f}});
  builder.begin_stack("text-layout", igr::Axis::vertical);
  builder.text("headline", "interop validation", "body-md");
  builder.text("body", "The DX12 backend should keep the DirectWrite interop path alive when atlas mode is disabled.", "body-md");
  builder.end_container();
  builder.end_container();
  return context.end_frame();
}



}  // namespace



int main() {

  const HINSTANCE instance = GetModuleHandleW(nullptr);

  const wchar_t* class_name = L"IGRDx12BackendTestWindow";



  WNDCLASSEXW window_class{};

  window_class.cbSize = sizeof(window_class);

  window_class.hInstance = instance;

  window_class.lpfnWndProc = test_window_proc;

  window_class.lpszClassName = class_name;



  if (RegisterClassExW(&window_class) == 0) {

    return fail("failed to register DX12 test window class");

  }



  HWND window_handle = CreateWindowExW(

      0,

      class_name,

      L"IGR DX12 Backend Test",

      WS_OVERLAPPEDWINDOW,

      CW_USEDEFAULT,

      CW_USEDEFAULT,

      320,

      240,

      nullptr,

      nullptr,

      instance,

      nullptr);



  if (window_handle == nullptr) {

    UnregisterClassW(class_name, instance);

    return fail("failed to create DX12 test window");

  }



  igr::backends::Dx12Backend backend({
      .diagnostics = {
          .memory_sample_interval = 8,
      },
      .resource_budgets = {
          .max_cached_wide_strings = 0,
      },

      .window_handle = window_handle,

      .initial_viewport = {320, 240},

      .enable_debug_layer = true,

      .enable_vsync = false,
      .text_renderer = igr::backends::Dx12TextRendererMode::atlas,

  });



  igr::Status status = backend.initialize();

  if (!status) {

    std::cerr << status.message() << '\n';

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend initialization failed");

  }



  status = backend.register_font("invalid-font", {

      .family = "Segoe UI",

      .size = 0.0f,

  });

  if (status) {

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend accepted an invalid font descriptor");

  }



  status = backend.register_image("invalid-image", {

      .texture_key = "texture-0",

      .size = {-1.0f, 48.0f},

  });

  if (status) {

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend accepted an invalid image descriptor");

  }



  const NullTextureBinding non_shader_visible_texture = create_texture_binding(

      backend.device_handle(),

      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,

      D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

  if (non_shader_visible_texture.heap) {

    status = backend.register_texture("non-shader-visible", non_shader_visible_texture.binding);

    if (status) {

      backend.shutdown();

      DestroyWindow(window_handle);

      UnregisterClassW(class_name, instance);

      return fail("DX12 backend accepted a non-shader-visible texture heap");

    }

  }



  const NullTextureBinding wrong_type_texture = create_texture_binding(

      backend.device_handle(),

      D3D12_DESCRIPTOR_HEAP_TYPE_RTV,

      D3D12_DESCRIPTOR_HEAP_FLAG_NONE);

  if (wrong_type_texture.heap) {

    status = backend.register_texture("wrong-heap-type", wrong_type_texture.binding);

    if (status) {

      backend.shutdown();

      DestroyWindow(window_handle);

      UnregisterClassW(class_name, instance);

      return fail("DX12 backend accepted a non-SRV descriptor heap");

    }

  }



  const NullTextureBinding missing_cpu_texture = create_texture_binding(

      backend.device_handle(),

      D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,

      D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,

      false,

      true);

  if (missing_cpu_texture.heap) {

    status = backend.register_texture("missing-cpu-descriptor", missing_cpu_texture.binding);

    if (status) {

      backend.shutdown();

      DestroyWindow(window_handle);

      UnregisterClassW(class_name, instance);

      return fail("DX12 backend accepted a texture binding without a CPU descriptor");

    }

  }



  status = backend.register_font("body-md", {

      .family = "Segoe UI",

      .size = 16.0f,

      .weight = igr::FontWeight::medium,

      .style = igr::FontStyle::normal,

      .locale = "en-us",

  });

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend font registration failed");

  }



  const NullTextureBinding null_texture = create_null_texture_binding(backend.device_handle());

  if (!null_texture.heap || null_texture.binding.gpu_descriptor.ptr == 0) {

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend test texture setup failed");

  }



  status = backend.register_texture("texture-0", null_texture.binding);

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend texture registration failed");

  }



  status = backend.register_image("preview-card", {

      .texture_key = "texture-0",

      .size = {96.0f, 48.0f},

      .uv = {{0.0f, 0.0f}, {1.0f, 1.0f}},

      .tint = {0.95f, 0.98f, 1.0f, 1.0f},

  });

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend image registration failed");

  }

  status = backend.register_shader("pulse-shader", make_demo_shader());

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend shader registration failed");

  }



  const igr::FrameDocument document = build_document(1, {320, 240});

  status = backend.render(document);

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend render failed");

  }



  status = backend.present();

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend present failed");

  }



  const auto initial_stats = backend.frame_stats();

  if (initial_stats.widget_count != document.widget_count() || initial_stats.quad_count == 0 || initial_stats.draw_batch_count == 0 ||
      initial_stats.label_count == 0 || initial_stats.textured_batch_count == 0 || initial_stats.shader_batch_count == 0) {

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend frame stats did not capture the first owned-window frame");

  }

  const auto initial_telemetry = backend.telemetry();

  if (initial_telemetry.frame.widget_count != document.widget_count() || initial_telemetry.resources.font_count == 0 ||
      initial_telemetry.resources.image_count == 0 || initial_telemetry.resources.shader_count == 0 ||
      initial_telemetry.resources.texture_count == 0 || initial_telemetry.resources.wide_text_cache_bytes != 0 ||
      !initial_telemetry.resources.text_atlas_active || initial_telemetry.scopes.empty()) {

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend telemetry did not capture the expected resource or scope diagnostics");

  }



  status = backend.resize({480, 320});

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend resize failed");

  }



  const igr::FrameDocument resized_document = build_document(2, {480, 320});

  status = backend.render(resized_document);

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend render after resize failed");

  }



  status = backend.present();

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend present after resize failed");

  }

  status = backend.resize({0, 0});

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend zero-sized resize failed");

  }

  status = backend.resize({480, 320});

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend restore after zero-sized resize failed");

  }

  status = backend.render(resized_document);

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend render after zero-sized restore failed");

  }

  status = backend.present();

  if (!status) {

    std::cerr << status.message() << '\n';

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend present after zero-sized restore failed");

  }



  const auto capabilities = backend.capabilities();

  if (!backend.initialized() || !backend.owns_device_objects() || !backend.has_font("body-md") || !backend.has_texture("texture-0") ||

      !backend.has_image("preview-card") || backend.last_widget_count() != resized_document.widget_count() ||

      !capabilities.manual_host_binding || !capabilities.user_textures || !capabilities.user_shaders) {

    backend.shutdown();

    DestroyWindow(window_handle);

    UnregisterClassW(class_name, instance);

    return fail("DX12 backend runtime state did not match expectations");

  }



  backend.shutdown();

  igr::backends::Dx12Backend interop_backend({
      .window_handle = window_handle,
      .initial_viewport = {320, 240},
      .enable_debug_layer = true,
      .enable_vsync = false,
      .text_renderer = igr::backends::Dx12TextRendererMode::interop,
  });

  status = interop_backend.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("DX12 backend interop initialization failed");
  }

  status = interop_backend.register_font("body-md", {
      .family = "Segoe UI",
      .size = 16.0f,
      .weight = igr::FontWeight::medium,
      .style = igr::FontStyle::normal,
      .locale = "en-us",
  });
  if (!status) {
    std::cerr << status.message() << '\n';
    interop_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("DX12 backend interop font registration failed");
  }

  const igr::FrameDocument interop_document = build_text_only_document(3, {320, 240});
  status = interop_backend.render(interop_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    interop_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("DX12 backend interop render failed");
  }

  status = interop_backend.present();
  if (!status) {
    std::cerr << status.message() << '\n';
    interop_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("DX12 backend interop present failed");
  }

  const auto interop_telemetry = interop_backend.telemetry();
  if (interop_telemetry.frame.widget_count != interop_document.widget_count() ||
      interop_telemetry.resources.text_atlas_active ||
      !interop_telemetry.resources.text_interop_active) {
    interop_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("DX12 backend interop telemetry did not report the expected text path");
  }

  interop_backend.shutdown();

  DestroyWindow(window_handle);

  UnregisterClassW(class_name, instance);



  std::cout << "igr_dx12_backend_tests passed" << '\n';

  return 0;

}
