#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include "igr/backend.hpp"
#include "igr/resources.hpp"

namespace igr::backends {

struct Dx12Theme {
  std::array<float, 4> window_background{0.13f, 0.15f, 0.18f, 0.96f};
  std::array<float, 4> title_bar{0.22f, 0.51f, 0.92f, 1.0f};
  std::array<float, 4> panel_background{0.09f, 0.10f, 0.13f, 0.92f};
  std::array<float, 4> text_primary{0.94f, 0.95f, 0.98f, 1.0f};
  std::array<float, 4> text_secondary{0.72f, 0.76f, 0.84f, 1.0f};
  std::array<float, 4> text_placeholder{0.18f, 0.21f, 0.26f, 0.78f};
  std::array<float, 4> text_accent{0.26f, 0.68f, 0.98f, 1.0f};
  std::array<float, 4> button_background{0.19f, 0.49f, 0.84f, 0.96f};
  std::array<float, 4> button_highlight{0.24f, 0.58f, 0.96f, 0.96f};
  std::array<float, 4> checkbox_border{0.58f, 0.63f, 0.73f, 0.95f};
  std::array<float, 4> checkbox_fill{0.20f, 0.43f, 0.86f, 1.0f};
  std::array<float, 4> progress_track{0.22f, 0.25f, 0.31f, 1.0f};
  std::array<float, 4> progress_fill{0.26f, 0.75f, 0.57f, 1.0f};
  std::array<float, 4> separator{0.45f, 0.48f, 0.54f, 0.70f};
  float window_title_height{28.0f};
  float padding{18.0f};
  float item_spacing{8.0f};
  float body_text_size{14.0f};
  float title_text_size{15.0f};
};

struct Dx12TextureBinding {
  ID3D12DescriptorHeap* heap{};
  D3D12_CPU_DESCRIPTOR_HANDLE cpu_descriptor{};
  D3D12_GPU_DESCRIPTOR_HANDLE gpu_descriptor{};
};

enum class Dx12TextRendererMode {
  atlas,
  interop,
};

struct Dx12TextAtlasConfig {
  std::uint32_t max_dimension{2048};
  std::uint32_t padding{1};
};

struct Dx12HostBinding {
  HWND window_handle{};
  ExtentU viewport{};
  ID3D12Device* device{};
  ID3D12CommandQueue* command_queue{};
  ID3D12GraphicsCommandList* command_list{};
  IDXGISwapChain3* swap_chain{};
};

struct Dx12FrameBinding {
  std::uint32_t frame_index{};
  std::uint32_t back_buffer_index{};
  ID3D12GraphicsCommandList* command_list{};
  ID3D12Resource* render_target{};
  D3D12_CPU_DESCRIPTOR_HANDLE render_target_view{};
  ID3D12DescriptorHeap* shader_visible_srv_heap{};
  bool host_sets_render_target{};
  bool host_sets_descriptor_heaps{};
  bool host_transitions_render_target{true};
  D3D12_RESOURCE_STATES render_target_state_before{D3D12_RESOURCE_STATE_PRESENT};
  D3D12_RESOURCE_STATES render_target_state_after{D3D12_RESOURCE_STATE_PRESENT};
  bool clear_target{};
};

struct Dx12BackendConfig {
  BackendHostOptions host{};
  DiagnosticsConfig diagnostics{};
  ResourceBudgetConfig resource_budgets{};
  HWND window_handle{};
  ExtentU initial_viewport{1280, 720};
  ID3D12Device* device{};
  ID3D12CommandQueue* command_queue{};
  ID3D12GraphicsCommandList* command_list{};
  IDXGISwapChain3* swap_chain{};
  bool enable_debug_layer{false};
  bool enable_vsync{true};
  Dx12TextRendererMode text_renderer{Dx12TextRendererMode::interop};
  Dx12TextAtlasConfig text_atlas{};
  std::uint32_t text_interop_idle_frame_threshold{120};
  std::array<float, 4> clear_color{0.05f, 0.06f, 0.08f, 1.0f};
  Dx12Theme theme{};
};

class Dx12Backend final : public IRendererBackend, public IResourceRegistry {
 public:
  class Impl;

  explicit Dx12Backend(Dx12BackendConfig config = {});
  ~Dx12Backend() override;

  [[nodiscard]] BackendKind kind() const noexcept override;
  [[nodiscard]] std::string_view name() const noexcept override;
  [[nodiscard]] BackendCapabilities capabilities() const noexcept override;
  [[nodiscard]] BackendFrameStats frame_stats() const noexcept override;
  [[nodiscard]] BackendTelemetrySnapshot telemetry() const noexcept override;

  Status initialize() override;
  void invalidate_back_buffer_resources() noexcept override;
  Status resize(ExtentU viewport) override;
  Status render(const FrameDocument& document) override;
  Status present() override;
  Status register_font(std::string_view key, const FontResourceDesc& descriptor) override;
  void unregister_font(std::string_view key) noexcept override;
  Status register_image(std::string_view key, const ImageResourceDesc& descriptor) override;
  void unregister_image(std::string_view key) noexcept override;
  Status register_shader(std::string_view key, const ShaderResourceDesc& descriptor) override;
  void unregister_shader(std::string_view key) noexcept override;
  void shutdown() noexcept override;
  Status bind_frame(const Dx12FrameBinding& binding);
  Status register_texture(std::string_view key, const Dx12TextureBinding& binding);
  void unregister_texture(std::string_view key) noexcept;
  void clear_frame_binding() noexcept;
  Status rebind_host(const Dx12HostBinding& binding);
  void detach_host() noexcept;

  [[nodiscard]] std::size_t last_widget_count() const noexcept;
  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool owns_device_objects() const noexcept;
  [[nodiscard]] bool has_frame_binding() const noexcept;
  [[nodiscard]] bool has_font(std::string_view key) const noexcept;
  [[nodiscard]] bool has_image(std::string_view key) const noexcept;
  [[nodiscard]] bool has_shader(std::string_view key) const noexcept;
  [[nodiscard]] bool has_texture(std::string_view key) const noexcept;
  [[nodiscard]] HWND window_handle() const noexcept;
  [[nodiscard]] ID3D12Device* device_handle() const noexcept;
  [[nodiscard]] ID3D12CommandQueue* command_queue_handle() const noexcept;
  [[nodiscard]] ID3D12GraphicsCommandList* command_list_handle() const noexcept;
  [[nodiscard]] IDXGISwapChain3* swap_chain_handle() const noexcept;

 private:
  Status create_device_objects();
  Status create_text_interop();
  Status ensure_text_bitmap_surface(std::uint32_t width, std::uint32_t height);
  Status ensure_text_atlas_resources(std::uint32_t width, std::uint32_t height);
  Status populate_text_atlas(const Dx12FrameBinding* active_binding);
  Status realize_registered_fonts();
  Status realize_registered_shaders();
  Status rebuild_text_targets();
  Status upload_text_atlas(ID3D12GraphicsCommandList* command_list);
  bool should_use_text_atlas(const Dx12FrameBinding* active_binding) const noexcept;
  void refresh_telemetry() noexcept;
  void clear_owned_frame_submission_state() noexcept;
  void release_transient_storage() noexcept;
  void reset_device_objects(bool clear_textures) noexcept;
  Dx12BackendConfig config_{};
  ExtentU viewport_{};
  std::size_t last_widget_count_{};
  bool initialized_{false};
  bool owns_device_objects_{false};
  bool has_frame_binding_{false};
  Dx12FrameBinding frame_binding_{};
  std::unique_ptr<Impl> impl_;
};

}  // namespace igr::backends
