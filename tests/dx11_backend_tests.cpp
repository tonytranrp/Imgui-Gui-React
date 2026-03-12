#include <Windows.h>

#include <array>
#include <iostream>
#include <iterator>
#include <wrl/client.h>

#include "igr/backends/dx11_backend.hpp"
#include "igr/context.hpp"

namespace {

using Microsoft::WRL::ComPtr;

struct ExternalDx11Host {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swap_chain;
};

ComPtr<ID3D11Device> create_standalone_dx11_device() {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  HRESULT hr = D3D11CreateDevice(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      flags,
      levels,
      static_cast<UINT>(std::size(levels)),
      D3D11_SDK_VERSION,
      &device,
      nullptr,
      &context);
  if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &context);
  }
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &context);
  }
  return SUCCEEDED(hr) ? device : ComPtr<ID3D11Device>{};
}

LRESULT CALLBACK test_window_proc(HWND window_handle, UINT message, WPARAM w_param, LPARAM l_param) {
  return DefWindowProcW(window_handle, message, w_param, l_param);
}

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

ComPtr<ID3D11ShaderResourceView> create_demo_texture(ID3D11Device* device) {
  if (device == nullptr) {
    return {};
  }

  static constexpr std::array<std::uint32_t, 16> kPixels{
      0xFF1F3658u, 0xFF3A6EA5u, 0xFF50A8D8u, 0xFF8FDBFFu,
      0xFF14324Bu, 0xFF2E5B7Eu, 0xFF3E84B0u, 0xFF6BC7F2u,
      0xFF0F2636u, 0xFF24485Eu, 0xFF2F6E8Au, 0xFF4CA7C9u,
      0xFF081925u, 0xFF173447u, 0xFF1F5B70u, 0xFF2F8DA8u,
  };

  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_desc.Width = 4;
  texture_desc.Height = 4;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA initial_data{};
  initial_data.pSysMem = kPixels.data();
  initial_data.SysMemPitch = 4 * sizeof(std::uint32_t);

  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&texture_desc, &initial_data, &texture))) {
    return {};
  }

  ComPtr<ID3D11ShaderResourceView> shader_resource_view;
  if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &shader_resource_view))) {
    return {};
  }

  return shader_resource_view;
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
  const float pulse = 0.45 + 0.55 * sin(igrParam0.x * 0.25 + input.uv.y * 6.28318);
  return float4(input.uv.x, pulse, input.uv.y, 1.0) * igrTint * input.color;
}
)";
  shader.samples_texture = false;
  shader.blend_mode = igr::ShaderBlendMode::alpha;
  return shader;
}

bool create_external_dx11_host(HWND window_handle, ExternalDx11Host& host) {
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
  DXGI_SWAP_CHAIN_DESC swap_chain_desc{};
  swap_chain_desc.BufferCount = 2;
  swap_chain_desc.BufferDesc.Width = 320;
  swap_chain_desc.BufferDesc.Height = 240;
  swap_chain_desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.OutputWindow = window_handle;
  swap_chain_desc.SampleDesc.Count = 1;
  swap_chain_desc.Windowed = TRUE;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      flags,
      levels,
      static_cast<UINT>(std::size(levels)),
      D3D11_SDK_VERSION,
      &swap_chain_desc,
      &host.swap_chain,
      &host.device,
      nullptr,
      &host.context);

  if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) {
    flags &= ~D3D11_CREATE_DEVICE_DEBUG;
    hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        &host.swap_chain,
        &host.device,
        nullptr,
        &host.context);
  }

  if (FAILED(hr)) {
    hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &swap_chain_desc,
        &host.swap_chain,
        &host.device,
        nullptr,
        &host.context);
  }

  return SUCCEEDED(hr);
}

}  // namespace

int main() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t* class_name = L"IGRDx11BackendTestWindow";

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpfnWndProc = test_window_proc;
  window_class.lpszClassName = class_name;

  if (RegisterClassExW(&window_class) == 0) {
    return fail("failed to register hidden test window class");
  }

  HWND window_handle = CreateWindowExW(
      0,
      class_name,
      L"IGR DX11 Backend Test",
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
    return fail("failed to create hidden test window");
  }

  igr::backends::Dx11Backend backend({
      .diagnostics = {
          .enabled = true,
          .memory_sample_interval = 8,
      },
      .resource_budgets = {
          .max_cached_wide_strings = 0,
      },
      .window_handle = window_handle,
      .initial_viewport = {320, 240},
      .enable_debug_layer = true,
      .enable_vsync = false,
  });

  igr::Status status = backend.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend initialization failed");
  }

  status = backend.register_font("invalid-font", {
      .family = "Segoe UI",
      .size = 0.0f,
  });
  if (status) {
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend accepted an invalid DX11 font descriptor");
  }

  status = backend.register_image("invalid-image", {
      .texture_key = "demo-texture",
      .size = {-4.0f, 72.0f},
  });
  if (status) {
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend accepted an invalid DX11 image descriptor");
  }

  const auto demo_texture = create_demo_texture(backend.device_handle());
  if (!demo_texture) {
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to create a demo texture for DX11 image validation");
  }

  status = backend.register_texture("demo-texture", {.shader_resource_view = demo_texture.Get()});
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend texture registration failed");
  }

  const auto foreign_device = create_standalone_dx11_device();
  const auto foreign_texture = create_demo_texture(foreign_device.Get());
  if (foreign_texture) {
    status = backend.register_texture("foreign-texture", {.shader_resource_view = foreign_texture.Get()});
    if (status) {
      backend.shutdown();
      DestroyWindow(window_handle);
      UnregisterClassW(class_name, instance);
      return fail("backend accepted a DX11 texture from a foreign device");
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
    return fail("backend font registration failed");
  }

  status = backend.register_image("demo-card", {
      .texture_key = "demo-texture",
      .size = {96.0f, 48.0f},
      .uv = {{0.0f, 0.0f}, {1.0f, 1.0f}},
      .tint = {0.94f, 0.97f, 1.0f, 1.0f},
  });
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend image registration failed");
  }

  status = backend.register_shader("pulse-shader", make_demo_shader());
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend shader registration failed");
  }

  igr::UiContext context;
  context.begin_frame({
      .frame_index = 1,
      .viewport = {320, 240},
      .delta_seconds = 1.0 / 60.0,
  });

  auto& builder = context.builder();
  builder.begin_window("dx11-test", "DX11 Test", {{16.0f, 16.0f}, {220.0f, 160.0f}});
  builder.begin_stack("layout", igr::Axis::vertical);
  builder.text("label", "runtime path", "body-md");
  builder.button("button", "smoke");
  builder.begin_clip_rect("preview-clip", {108.0f, 84.0f});
  builder.checkbox("checkbox", "Debug overlay", true);
  builder.image("preview", "demo-texture", {96.0f, 48.0f}, "Texture preview", "demo-card");
  igr::ShaderUniformData shader_uniforms{};
  shader_uniforms.tint = {0.74f, 0.91f, 1.0f, 0.92f};
  shader_uniforms.params[0] = {1.0f, 1.0f / 60.0f, 320.0f, 240.0f};
  builder.shader_image("shader-preview", "pulse-shader", "demo-texture", {96.0f, 48.0f}, "Shader preview", "demo-card", shader_uniforms);
  builder.fill_rect("preview-fill", {{8.0f, 8.0f}, {20.0f, 8.0f}}, {0.25f, 0.70f, 0.98f, 0.85f});
  builder.draw_line("preview-line", {6.0f, 42.0f}, {92.0f, 42.0f}, {0.12f, 0.18f, 0.22f, 1.0f}, 2.0f);
  builder.end_container();
  builder.progress_bar("progress", "Upload progress", 0.65f);
  builder.end_container();
  builder.end_container();

  const igr::FrameDocument document = context.end_frame();

  status = backend.render(document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend render failed");
  }

  status = backend.present();
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend present failed");
  }

  const auto frame_stats = backend.frame_stats();
  if (frame_stats.widget_count != document.widget_count() || frame_stats.textured_batch_count == 0 || frame_stats.label_count == 0 ||
      frame_stats.shader_batch_count == 0) {
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend frame stats did not capture the first DX11 frame");
  }

  const auto telemetry = backend.telemetry();
  if (telemetry.frame.widget_count != document.widget_count() || telemetry.resources.font_count == 0 || telemetry.resources.image_count == 0 ||
      telemetry.resources.shader_count == 0 || telemetry.resources.texture_count == 0 || telemetry.resources.wide_text_cache_bytes != 0 ||
      telemetry.scopes.empty()) {
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend telemetry did not capture DX11 resource or scope diagnostics");
  }

  status = backend.resize({480, 320});
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend resize failed");
  }

  context.begin_frame({
      .frame_index = 2,
      .viewport = {480, 320},
      .delta_seconds = 1.0 / 60.0,
  });

  auto& resized_builder = context.builder();
  resized_builder.begin_window("dx11-test", "DX11 Test", {{24.0f, 24.0f}, {280.0f, 180.0f}});
  resized_builder.begin_stack("layout", igr::Axis::vertical);
  resized_builder.text("label", "resized runtime path", "body-md");
  resized_builder.begin_clip_rect("preview-clip", {132.0f, 92.0f});
  resized_builder.checkbox("checkbox", "Debug overlay", false);
  resized_builder.image("preview", "demo-texture", {120.0f, 56.0f}, "Texture preview", "demo-card");
  resized_builder.shader_image("shader-preview", "pulse-shader", "demo-texture", {120.0f, 56.0f}, "Shader preview", "demo-card");
  resized_builder.stroke_rect("preview-outline", {{4.0f, 4.0f}, {110.0f, 48.0f}}, {0.22f, 0.78f, 0.59f, 1.0f}, 2.0f);
  resized_builder.end_container();
  resized_builder.progress_bar("progress", "Upload progress", 1.5f);
  resized_builder.end_container();
  resized_builder.end_container();

  const igr::FrameDocument resized_document = context.end_frame();
  status = backend.render(resized_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend render after resize failed");
  }

  status = backend.present();
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend present after resize failed");
  }

  status = backend.resize({0, 0});
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend zero-sized resize failed");
  }

  status = backend.resize({480, 320});
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend restore after zero-sized resize failed");
  }

  status = backend.render(resized_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend render after zero-sized restore failed");
  }

  status = backend.present();
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend present after zero-sized restore failed");
  }

  if (!backend.initialized() || !backend.owns_device_objects() || !backend.has_texture("demo-texture") || !backend.has_font("body-md") ||
      !backend.has_image("demo-card") ||
      backend.last_widget_count() != resized_document.widget_count()) {
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend runtime state did not match expectations");
  }

  const auto capabilities = backend.capabilities();
  if (!capabilities.user_textures || !capabilities.user_shaders || !capabilities.manual_host_binding || !capabilities.host_state_restore ||
      !capabilities.injected_overlay) {
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("backend capabilities did not advertise the expected DX11 renderer features");
  }

  backend.shutdown();

  ExternalDx11Host external_host;
  if (!create_external_dx11_host(window_handle, external_host)) {
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to create external DX11 resources for overlay-mode validation");
  }

  igr::backends::Dx11Backend overlay_backend({
      .host = {
          .host_mode = igr::HostMode::injected_overlay,
          .presentation_mode = igr::PresentationMode::host_managed,
          .resize_mode = igr::ResizeMode::host_managed,
          .input_mode = igr::InputMode::none,
          .restore_host_state = true,
      },
      .swap_chain = external_host.swap_chain.Get(),
      .enable_debug_layer = true,
      .enable_vsync = false,
  });

  status = overlay_backend.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend initialization failed");
  }

  if (overlay_backend.owns_device_objects()) {
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend should not report ownership of externally supplied resources");
  }

  const auto overlay_demo_texture = create_demo_texture(external_host.device.Get());
  if (!overlay_demo_texture) {
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to create a demo texture for DX11 overlay validation");
  }

  status = overlay_backend.register_texture("overlay-demo-texture", {.shader_resource_view = overlay_demo_texture.Get()});
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend texture registration failed");
  }

  status = overlay_backend.register_font("overlay-body-md", {
      .family = "Segoe UI",
      .size = 16.0f,
      .weight = igr::FontWeight::medium,
      .style = igr::FontStyle::normal,
      .locale = "en-us",
  });
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend font registration failed");
  }

  status = overlay_backend.register_image("overlay-demo-card", {
      .texture_key = "overlay-demo-texture",
      .size = {96.0f, 48.0f},
      .uv = {{0.0f, 0.0f}, {1.0f, 1.0f}},
      .tint = {0.94f, 0.97f, 1.0f, 1.0f},
  });
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend image registration failed");
  }

  status = overlay_backend.register_shader("pulse-shader", make_demo_shader());
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend shader registration failed");
  }

  igr::UiContext overlay_context;
  overlay_context.begin_frame({
      .frame_index = 3,
      .viewport = {320, 240},
      .delta_seconds = 1.0 / 60.0,
  });
  auto& overlay_builder = overlay_context.builder();
  overlay_builder.begin_window("dx11-overlay", "DX11 Overlay", {{16.0f, 16.0f}, {220.0f, 160.0f}});
  overlay_builder.begin_stack("layout", igr::Axis::vertical);
  overlay_builder.text("label", "overlay runtime path", "overlay-body-md");
  overlay_builder.button("button", "smoke");
  overlay_builder.begin_clip_rect("preview-clip", {108.0f, 84.0f});
  overlay_builder.checkbox("checkbox", "Debug overlay", true);
  overlay_builder.image("preview", "overlay-demo-texture", {96.0f, 48.0f}, "Texture preview", "overlay-demo-card");
  overlay_builder.shader_image("shader-preview", "pulse-shader", "overlay-demo-texture", {96.0f, 48.0f}, "Shader preview", "overlay-demo-card");
  overlay_builder.fill_rect("preview-fill", {{8.0f, 8.0f}, {20.0f, 8.0f}}, {0.25f, 0.70f, 0.98f, 0.85f});
  overlay_builder.draw_line("preview-line", {6.0f, 42.0f}, {92.0f, 42.0f}, {0.12f, 0.18f, 0.22f, 1.0f}, 2.0f);
  overlay_builder.end_container();
  overlay_builder.progress_bar("progress", "Upload progress", 0.65f);
  overlay_builder.end_container();
  overlay_builder.end_container();
  const igr::FrameDocument overlay_document = overlay_context.end_frame();

  overlay_context.begin_frame({
      .frame_index = 4,
      .viewport = {480, 320},
      .delta_seconds = 1.0 / 60.0,
  });
  auto& overlay_resized_builder = overlay_context.builder();
  overlay_resized_builder.begin_window("dx11-overlay", "DX11 Overlay", {{24.0f, 24.0f}, {280.0f, 180.0f}});
  overlay_resized_builder.begin_stack("layout", igr::Axis::vertical);
  overlay_resized_builder.text("label", "overlay resized runtime path", "overlay-body-md");
  overlay_resized_builder.begin_clip_rect("preview-clip", {132.0f, 92.0f});
  overlay_resized_builder.checkbox("checkbox", "Debug overlay", false);
  overlay_resized_builder.image("preview", "overlay-demo-texture", {120.0f, 56.0f}, "Texture preview", "overlay-demo-card");
  overlay_resized_builder.shader_image("shader-preview", "pulse-shader", "overlay-demo-texture", {120.0f, 56.0f}, "Shader preview", "overlay-demo-card");
  overlay_resized_builder.stroke_rect("preview-outline", {{4.0f, 4.0f}, {110.0f, 48.0f}}, {0.22f, 0.78f, 0.59f, 1.0f}, 2.0f);
  overlay_resized_builder.end_container();
  overlay_resized_builder.progress_bar("progress", "Upload progress", 1.0f);
  overlay_resized_builder.end_container();
  overlay_resized_builder.end_container();
  const igr::FrameDocument overlay_resized_document = overlay_context.end_frame();

  D3D11_RASTERIZER_DESC rasterizer_desc{};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_FRONT;
  rasterizer_desc.DepthClipEnable = TRUE;
  ComPtr<ID3D11RasterizerState> custom_rasterizer_state;
  if (FAILED(external_host.device->CreateRasterizerState(&rasterizer_desc, &custom_rasterizer_state))) {
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to create a custom rasterizer state for overlay-mode validation");
  }

  const D3D11_VIEWPORT custom_viewport{0.0f, 0.0f, 111.0f, 77.0f, 0.0f, 1.0f};
  external_host.context->RSSetState(custom_rasterizer_state.Get());
  external_host.context->RSSetViewports(1, &custom_viewport);
  external_host.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);

  status = overlay_backend.render(overlay_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend render failed");
  }

  status = overlay_backend.present();
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend host-managed present failed");
  }

  ComPtr<ID3D11RasterizerState> restored_rasterizer_state;
  external_host.context->RSGetState(&restored_rasterizer_state);
  if (restored_rasterizer_state.Get() != custom_rasterizer_state.Get()) {
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend did not restore the host rasterizer state");
  }

  UINT viewport_count = 1;
  D3D11_VIEWPORT restored_viewport{};
  external_host.context->RSGetViewports(&viewport_count, &restored_viewport);
  if (viewport_count != 1 || restored_viewport.Width != custom_viewport.Width || restored_viewport.Height != custom_viewport.Height) {
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend did not restore the host viewport");
  }

  D3D11_PRIMITIVE_TOPOLOGY restored_topology{};
  external_host.context->IAGetPrimitiveTopology(&restored_topology);
  if (restored_topology != D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP) {
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend did not restore the host primitive topology");
  }

  overlay_backend.invalidate_back_buffer_resources();
  if (FAILED(external_host.swap_chain->ResizeBuffers(0, 480, 320, DXGI_FORMAT_UNKNOWN, 0))) {
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("host-managed resize buffer simulation failed");
  }

  status = overlay_backend.resize({480, 320});
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend resize failed");
  }

  status = overlay_backend.render(overlay_resized_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay-mode backend render after resize failed");
  }

  overlay_backend.shutdown();
  DestroyWindow(window_handle);
  UnregisterClassW(class_name, instance);

  std::cout << "igr_dx11_backend_tests passed" << '\n';
  return 0;
}

