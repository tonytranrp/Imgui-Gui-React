#include <Windows.h>

#include <array>
#include <iostream>
#include <wrl/client.h>

#include "igr/backends/dx11_backend.hpp"
#include "react_native_fixture.hpp"

namespace {

using Microsoft::WRL::ComPtr;

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

}  // namespace

int main() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t* class_name = L"IGRDx11ExternalHostTestWindow";

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpfnWndProc = test_window_proc;
  window_class.lpszClassName = class_name;

  if (RegisterClassExW(&window_class) == 0) {
    return fail("failed to register external-host test window class");
  }

  HWND window_handle = CreateWindowExW(
      0,
      class_name,
      L"IGR DX11 External Host Test",
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
    return fail("failed to create external-host test window");
  }

  igr::backends::Dx11Backend owner_backend({
      .window_handle = window_handle,
      .initial_viewport = {320, 240},
      .enable_debug_layer = true,
      .enable_vsync = false,
  });

  igr::Status status = owner_backend.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("owner backend initialization failed");
  }

  igr::backends::Dx11Backend overlay_backend({
      .host =
          {
              .host_mode = igr::HostMode::injected_overlay,
              .presentation_mode = igr::PresentationMode::host_managed,
              .resize_mode = igr::ResizeMode::host_managed,
              .input_mode = igr::InputMode::none,
              .clear_target = false,
              .restore_host_state = true,
          },
      .device = owner_backend.device_handle(),
      .device_context = owner_backend.device_context_handle(),
      .swap_chain = owner_backend.swap_chain_handle(),
      .derive_device_from_swap_chain = false,
      .derive_window_from_swap_chain = true,
      .enable_debug_layer = false,
      .enable_vsync = false,
  });

  status = overlay_backend.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend initialization failed");
  }

  igr::react::TransportEnvelope envelope;
  igr::FrameDocument overlay_document;
  status = igr::tests::load_react_native_scene({
      .frame_index = 1,
      .viewport = {320, 240},
      .delta_seconds = 1.0 / 60.0,
  }, &envelope, &overlay_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to load the TSX-generated React scene");
  }

  if (envelope.session.host.host_mode != igr::HostMode::injected_overlay ||
      envelope.session.host.presentation_mode != igr::PresentationMode::host_managed ||
      envelope.images.empty() || envelope.fonts.empty()) {
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("React fixture metadata did not preserve the expected external-host session contract");
  }

  status = igr::tests::register_transport_resources(envelope, overlay_backend);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend resource registration from the React scene failed");
  }

  const ComPtr<ID3D11ShaderResourceView> demo_texture = create_demo_texture(owner_backend.device_handle());
  if (!demo_texture) {
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to create the DX11 texture binding for the React scene");
  }

  status = overlay_backend.register_texture("physics-gradient", {.shader_resource_view = demo_texture.Get()});
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend texture registration from the React scene failed");
  }

  status = overlay_backend.render(overlay_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend render failed");
  }

  status = overlay_backend.present();
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend host-managed present failed");
  }

  status = overlay_backend.resize({320, 240});
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend host-managed resize failed");
  }

  igr::react::TransportEnvelope resized_envelope;
  igr::FrameDocument resized_document;
  status = igr::tests::load_react_native_scene({
      .frame_index = 2,
      .viewport = {320, 240},
      .delta_seconds = 1.0 / 60.0,
  }, &resized_envelope, &resized_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to refresh the React scene after resize");
  }

  status = overlay_backend.render(resized_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend render failed after loading a fresh runtime scene");
  }

  if (!overlay_backend.initialized() || overlay_backend.owns_device_objects() || overlay_backend.last_widget_count() != resized_document.widget_count()) {
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend runtime state did not match expectations");
  }

  overlay_backend.detach_host();
  if (overlay_backend.initialized()) {
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend detach_host should leave the backend uninitialized");
  }

  status = overlay_backend.rebind_host({
      .window_handle = window_handle,
      .viewport = {320, 240},
      .device = owner_backend.device_handle(),
      .device_context = owner_backend.device_context_handle(),
      .swap_chain = owner_backend.swap_chain_handle(),
      .derive_device_from_swap_chain = false,
      .derive_window_from_swap_chain = true,
  });
  if (!status) {
    std::cerr << status.message() << '\n';
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend rebind_host failed");
  }

  igr::react::TransportEnvelope rebound_envelope;
  igr::FrameDocument rebound_document;
  status = igr::tests::load_react_native_scene({
      .frame_index = 3,
      .viewport = {320, 240},
      .delta_seconds = 1.0 / 60.0,
  }, &rebound_envelope, &rebound_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to refresh the React scene after host rebind");
  }

  status = igr::tests::register_transport_resources(rebound_envelope, overlay_backend);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend resource reregistration failed after rebind");
  }

  status = overlay_backend.register_texture("physics-gradient", {.shader_resource_view = demo_texture.Get()});
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend texture reregistration failed after rebind");
  }

  status = overlay_backend.render(rebound_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend render failed after detach/rebind");
  }

  const auto capabilities = overlay_backend.capabilities();
  if (!capabilities.manual_host_binding || !capabilities.injected_overlay || !capabilities.host_state_restore) {
    overlay_backend.shutdown();
    owner_backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("overlay backend capabilities did not advertise the expected host integration support");
  }

  overlay_backend.shutdown();
  owner_backend.shutdown();
  DestroyWindow(window_handle);
  UnregisterClassW(class_name, instance);

  std::cout << "igr_dx11_external_host_tests passed" << '\n';
  return 0;
}
