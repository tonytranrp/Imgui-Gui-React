#include <Windows.h>

#include <iostream>
#include <wrl/client.h>

#include "igr/backends/dx12_backend.hpp"
#include "igr/context.hpp"
#include "react_native_fixture.hpp"

namespace {

using Microsoft::WRL::ComPtr;

struct ExternalDx12Host {
  ComPtr<IDXGIFactory4> factory;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<IDXGISwapChain3> swap_chain;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  ComPtr<ID3D12DescriptorHeap> frame_srv_heap;
  ComPtr<ID3D12DescriptorHeap> alternate_srv_heap;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> command_list;
  ComPtr<ID3D12Resource> back_buffer;
  D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};

LRESULT CALLBACK test_window_proc(HWND window_handle, UINT message, WPARAM w_param, LPARAM l_param) {
  return DefWindowProcW(window_handle, message, w_param, l_param);
}

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

bool create_external_dx12_host(HWND window_handle, ExternalDx12Host& host) {
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&host.factory)))) {
    return false;
  }

  HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.device));
  if (FAILED(hr)) {
    ComPtr<IDXGIAdapter> warp_adapter;
    if (FAILED(host.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter)))) {
      return false;
    }
    hr = D3D12CreateDevice(warp_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host.device));
    if (FAILED(hr)) {
      return false;
    }
  }

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(host.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&host.queue)))) {
    return false;
  }

  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc{};
  swap_chain_desc.BufferCount = 2;
  swap_chain_desc.Width = 320;
  swap_chain_desc.Height = 240;
  swap_chain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swap_chain_desc.SampleDesc.Count = 1;

  ComPtr<IDXGISwapChain1> swap_chain1;
  if (FAILED(host.factory->CreateSwapChainForHwnd(host.queue.Get(), window_handle, &swap_chain_desc, nullptr, nullptr, &swap_chain1))) {
    return false;
  }
  host.factory->MakeWindowAssociation(window_handle, DXGI_MWA_NO_ALT_ENTER);
  if (FAILED(swap_chain1.As(&host.swap_chain))) {
    return false;
  }

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  if (FAILED(host.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&host.rtv_heap)))) {
    return false;
  }

  if (FAILED(host.swap_chain->GetBuffer(0, IID_PPV_ARGS(&host.back_buffer)))) {
    return false;
  }
  host.rtv = host.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  host.device->CreateRenderTargetView(host.back_buffer.Get(), nullptr, host.rtv);

  D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc{};
  srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_heap_desc.NumDescriptors = 1;
  srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(host.device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&host.frame_srv_heap)))) {
    return false;
  }
  if (FAILED(host.device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&host.alternate_srv_heap)))) {
    return false;
  }

  if (FAILED(host.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&host.allocator)))) {
    return false;
  }
  if (FAILED(host.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, host.allocator.Get(), nullptr, IID_PPV_ARGS(&host.command_list)))) {
    return false;
  }

  return true;
}

bool reset_host_command_list(ExternalDx12Host& host) {
  if (FAILED(host.command_list->Close())) {
    return false;
  }
  if (FAILED(host.allocator->Reset())) {
    return false;
  }
  if (FAILED(host.command_list->Reset(host.allocator.Get(), nullptr))) {
    return false;
  }

  ID3D12DescriptorHeap* heaps[] = {host.frame_srv_heap.Get()};
  host.command_list->SetDescriptorHeaps(1, heaps);
  return true;
}

}  // namespace

int main() {
  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t* class_name = L"IGRDx12ContractTestWindow";

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpfnWndProc = test_window_proc;
  window_class.lpszClassName = class_name;

  if (RegisterClassExW(&window_class) == 0) {
    return fail("failed to register DX12 contract test window class");
  }

  HWND window_handle = CreateWindowExW(
      0,
      class_name,
      L"IGR DX12 Contract Test",
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
    return fail("failed to create DX12 contract test window");
  }

  ExternalDx12Host host;
  if (!create_external_dx12_host(window_handle, host)) {
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return fail("failed to create external DX12 resources for the contract test");
  }

  igr::backends::Dx12Backend backend({
      .host = {
          .host_mode = igr::HostMode::injected_overlay,
          .presentation_mode = igr::PresentationMode::host_managed,
          .resize_mode = igr::ResizeMode::host_managed,
          .input_mode = igr::InputMode::none,
          .clear_target = false,
          .restore_host_state = false,
      },
      .device = host.device.Get(),
      .command_queue = host.queue.Get(),
      .swap_chain = host.swap_chain.Get(),
      .enable_debug_layer = false,
  });

  igr::Status status = backend.bind_frame({});
  if (status) {
    return fail("Dx12Backend bind_frame should reject calls before initialize");
  }

  status = backend.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("Dx12Backend initialization failed for contract test");
  }

  igr::react::TransportEnvelope envelope;
  igr::FrameDocument document;
  status = igr::tests::load_react_native_scene({
      .frame_index = 7,
      .viewport = {1280, 720},
      .delta_seconds = 1.0 / 60.0,
  }, &envelope, &document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend failed to load the TSX-generated React scene");
  }

  if (envelope.session.host.host_mode != igr::HostMode::injected_overlay ||
      envelope.session.host.presentation_mode != igr::PresentationMode::host_managed ||
      envelope.images.empty() || envelope.fonts.empty()) {
    backend.shutdown();
    return fail("Dx12Backend React scene metadata did not preserve the expected external-host contract");
  }

  status = igr::tests::register_transport_resources(envelope, backend);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend resource registration from the React scene failed");
  }
  status = backend.render(document);
  if (status) {
    backend.shutdown();
    return fail("Dx12Backend render should require a frame binding in host-managed modes");
  }

  igr::backends::Dx12FrameBinding invalid_binding{};
  invalid_binding.command_list = host.command_list.Get();
  invalid_binding.render_target = host.back_buffer.Get();
  status = backend.bind_frame(invalid_binding);
  if (status) {
    backend.shutdown();
    return fail("Dx12Backend bind_frame should reject a binding without RTV and descriptor heap");
  }

  igr::backends::Dx12FrameBinding valid_binding{};
  valid_binding.frame_index = 7;
  valid_binding.back_buffer_index = 0;
  valid_binding.command_list = host.command_list.Get();
  valid_binding.render_target = host.back_buffer.Get();
  valid_binding.render_target_view = host.rtv;
  valid_binding.shader_visible_srv_heap = host.frame_srv_heap.Get();
  valid_binding.host_sets_descriptor_heaps = true;
  valid_binding.host_sets_render_target = false;
  valid_binding.host_transitions_render_target = true;
  valid_binding.clear_target = false;

  status = backend.bind_frame(valid_binding);
  if (!status || !backend.has_frame_binding()) {
    if (!status) {
      std::cerr << status.message() << '\n';
    }
    backend.shutdown();
    return fail("Dx12Backend bind_frame rejected a valid host-managed binding");
  }

  igr::UiContext solid_context;
  solid_context.begin_frame({
      .frame_index = 6,
      .viewport = {320, 240},
      .delta_seconds = 1.0 / 60.0,
  });
  auto& solid_builder = solid_context.builder();
  solid_builder.begin_window("solid-only", "Solid Only", {{16.0f, 16.0f}, {180.0f, 128.0f}});
  solid_builder.begin_stack("layout", igr::Axis::vertical);
  solid_builder.button("button", "No SRV heap required");
  solid_builder.progress_bar("progress", "Solid batch", 0.4f);
  solid_builder.end_container();
  solid_builder.end_container();
  const igr::FrameDocument solid_document = solid_context.end_frame();

  igr::backends::Dx12FrameBinding solid_binding = valid_binding;
  solid_binding.shader_visible_srv_heap = nullptr;
  solid_binding.host_sets_descriptor_heaps = false;
  status = backend.bind_frame(solid_binding);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend should accept a solid-only host binding without an SRV heap");
  }

  if (!reset_host_command_list(host)) {
    backend.shutdown();
    return fail("failed to reset the host command list for the solid-only DX12 contract render");
  }

  status = backend.render(solid_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend solid-only host-managed render failed without an SRV heap");
  }

  status = backend.bind_frame(valid_binding);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend failed to restore the heap-managed binding after the solid-only render");
  }

  status = backend.render(document);
  if (status) {
    backend.shutdown();
    return fail("Dx12Backend render should reject image widgets without registered DX12 texture bindings");
  }

  igr::backends::Dx12TextureBinding invalid_texture_binding{};
  status = backend.register_texture("physics-gradient", invalid_texture_binding);
  if (status) {
    backend.shutdown();
    return fail("Dx12Backend should reject texture bindings without a heap and GPU descriptor");
  }

  igr::backends::Dx12TextureBinding wrong_heap_binding{};
  wrong_heap_binding.heap = host.alternate_srv_heap.Get();
  wrong_heap_binding.cpu_descriptor = host.alternate_srv_heap->GetCPUDescriptorHandleForHeapStart();
  wrong_heap_binding.gpu_descriptor = host.alternate_srv_heap->GetGPUDescriptorHandleForHeapStart();
  status = backend.register_texture("physics-gradient", wrong_heap_binding);
  if (!status || !backend.has_texture("physics-gradient")) {
    if (!status) {
      std::cerr << status.message() << '\n';
    }
    backend.shutdown();
    return fail("Dx12Backend rejected a structurally valid texture binding");
  }

  status = backend.render(document);
  if (status) {
    backend.shutdown();
    return fail("Dx12Backend should reject texture bindings from a different host-bound shader-visible heap");
  }

  igr::backends::Dx12TextureBinding valid_texture_binding{};
  valid_texture_binding.heap = valid_binding.shader_visible_srv_heap;
  valid_texture_binding.cpu_descriptor = host.frame_srv_heap->GetCPUDescriptorHandleForHeapStart();
  valid_texture_binding.gpu_descriptor = host.frame_srv_heap->GetGPUDescriptorHandleForHeapStart();
  status = backend.register_texture("physics-gradient", valid_texture_binding);
  if (!status || !backend.has_texture("physics-gradient")) {
    if (!status) {
      std::cerr << status.message() << '\n';
    }
    backend.shutdown();
    return fail("Dx12Backend rejected a valid descriptor-backed texture binding");
  }

  if (!reset_host_command_list(host)) {
    backend.shutdown();
    return fail("failed to reset the host command list for the DX12 contract render");
  }

  status = backend.render(document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend render rejected a valid frame binding");
  }

  if (backend.last_widget_count() != document.widget_count()) {
    backend.shutdown();
    return fail("Dx12Backend did not capture the frame document state");
  }

  const auto frame_stats = backend.frame_stats();
  if (frame_stats.widget_count != document.widget_count() || frame_stats.draw_batch_count == 0 || frame_stats.textured_batch_count == 0 ||
      frame_stats.label_count == 0 ||
      !backend.has_font("body-md") || !backend.has_image("physics-gradient-card")) {
    backend.shutdown();
    return fail("Dx12Backend contract path did not preserve runtime stats or registered resources");
  }

  if (!reset_host_command_list(host)) {
    backend.shutdown();
    return fail("failed to reset the DX12 host command list before detach/rebind validation");
  }

  backend.detach_host();
  if (backend.initialized() || backend.has_frame_binding()) {
    backend.shutdown();
    return fail("Dx12Backend detach_host should leave the backend uninitialized and clear the frame binding");
  }

  status = backend.rebind_host({
      .window_handle = window_handle,
      .viewport = {320, 240},
      .device = host.device.Get(),
      .command_queue = host.queue.Get(),
      .command_list = host.command_list.Get(),
      .swap_chain = host.swap_chain.Get(),
  });
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend rebind_host failed");
  }

  igr::react::TransportEnvelope rebound_envelope;
  igr::FrameDocument rebound_document;
  status = igr::tests::load_react_native_scene({
      .frame_index = 8,
      .viewport = {320, 240},
      .delta_seconds = 1.0 / 60.0,
  }, &rebound_envelope, &rebound_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend failed to refresh the React scene after host rebind");
  }

  status = igr::tests::register_transport_resources(rebound_envelope, backend);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend resource registration failed after host rebind");
  }

  igr::backends::Dx12FrameBinding rebound_binding{};
  rebound_binding.frame_index = 8;
  rebound_binding.back_buffer_index = 0;
  rebound_binding.command_list = host.command_list.Get();
  rebound_binding.render_target = host.back_buffer.Get();
  rebound_binding.render_target_view = host.rtv;
  rebound_binding.shader_visible_srv_heap = host.frame_srv_heap.Get();
  rebound_binding.host_sets_descriptor_heaps = true;
  rebound_binding.host_sets_render_target = false;
  rebound_binding.host_transitions_render_target = true;
  rebound_binding.clear_target = false;
  status = backend.bind_frame(rebound_binding);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend bind_frame failed after host rebind");
  }

  igr::backends::Dx12TextureBinding rebound_texture_binding{};
  rebound_texture_binding.heap = host.frame_srv_heap.Get();
  rebound_texture_binding.cpu_descriptor = host.frame_srv_heap->GetCPUDescriptorHandleForHeapStart();
  rebound_texture_binding.gpu_descriptor = host.frame_srv_heap->GetGPUDescriptorHandleForHeapStart();
  status = backend.register_texture("physics-gradient", rebound_texture_binding);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend texture registration failed after host rebind");
  }

  if (!reset_host_command_list(host)) {
    backend.shutdown();
    return fail("failed to reset the DX12 host command list after rebind");
  }

  status = backend.render(rebound_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    backend.shutdown();
    return fail("Dx12Backend render failed after host rebind");
  }

  backend.clear_frame_binding();
  if (backend.has_frame_binding()) {
    backend.shutdown();
    return fail("Dx12Backend clear_frame_binding did not clear the active binding");
  }

  backend.unregister_texture("physics-gradient");
  if (backend.has_texture("physics-gradient")) {
    backend.shutdown();
    return fail("Dx12Backend unregister_texture did not remove the descriptor-backed texture");
  }

  status = backend.render(rebound_document);
  if (status) {
    backend.shutdown();
    return fail("Dx12Backend render should fail again after clear_frame_binding");
  }

  backend.shutdown();
  DestroyWindow(window_handle);
  UnregisterClassW(class_name, instance);

  std::cout << "igr_dx12_contract_tests passed" << '\n';
  return 0;
}
