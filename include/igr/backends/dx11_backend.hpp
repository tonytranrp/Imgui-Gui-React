#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "igr/backend.hpp"
#include "igr/resources.hpp"

namespace igr::backends {

struct Dx11TextureBinding {
  ID3D11ShaderResourceView* shader_resource_view{};
};

struct Dx11HostBinding {
  HWND window_handle{};
  ExtentU viewport{};
  ID3D11Device* device{};
  ID3D11DeviceContext* device_context{};
  IDXGISwapChain* swap_chain{};
  bool derive_device_from_swap_chain{true};
  bool derive_window_from_swap_chain{true};
};

struct Dx11Theme {
  std::array<float, 4> window_background{0.14f, 0.16f, 0.20f, 0.96f};
  std::array<float, 4> title_bar{0.20f, 0.43f, 0.86f, 1.0f};
  std::array<float, 4> panel_background{0.11f, 0.12f, 0.15f, 0.92f};
  std::array<float, 4> text_primary{0.94f, 0.95f, 0.98f, 1.0f};
  std::array<float, 4> text_secondary{0.72f, 0.76f, 0.84f, 1.0f};
  std::array<float, 4> accent{0.28f, 0.62f, 0.97f, 1.0f};
  std::array<float, 4> button_background{0.18f, 0.49f, 0.84f, 0.96f};
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

struct Dx11BackendConfig {
  BackendHostOptions host{};
  HWND window_handle{};
  ExtentU initial_viewport{1280, 720};
  ID3D11Device* device{};
  ID3D11DeviceContext* device_context{};
  IDXGISwapChain* swap_chain{};
  bool derive_device_from_swap_chain{true};
  bool derive_window_from_swap_chain{true};
  bool enable_debug_layer{false};
  bool enable_vsync{true};
  std::array<float, 4> clear_color{0.09f, 0.10f, 0.13f, 1.0f};
  Dx11Theme theme{};
};

class Dx11Backend final : public IRendererBackend {
 public:
  explicit Dx11Backend(Dx11BackendConfig config = {});
  ~Dx11Backend() override;

  [[nodiscard]] BackendKind kind() const noexcept override;
  [[nodiscard]] std::string_view name() const noexcept override;
  [[nodiscard]] BackendCapabilities capabilities() const noexcept override;
  [[nodiscard]] BackendFrameStats frame_stats() const noexcept override;

  Status initialize() override;
  void invalidate_back_buffer_resources() noexcept override;
  Status resize(ExtentU viewport) override;
  Status render(const FrameDocument& document) override;
  Status present() override;
  Status register_font(std::string_view key, const FontResourceDesc& descriptor);
  void unregister_font(std::string_view key) noexcept;
  void shutdown() noexcept override;
  Status register_image(std::string_view key, const ImageResourceDesc& descriptor);
  void unregister_image(std::string_view key) noexcept;
  Status register_shader(std::string_view key, const ShaderResourceDesc& descriptor);
  void unregister_shader(std::string_view key) noexcept;
  Status register_texture(std::string_view key, const Dx11TextureBinding& binding);
  Status register_texture(std::string_view key, ID3D11ShaderResourceView* shader_resource_view);
  void unregister_texture(std::string_view key) noexcept;
  Status rebind_host(const Dx11HostBinding& binding);
  void detach_host() noexcept;

  [[nodiscard]] std::size_t last_widget_count() const noexcept;
  [[nodiscard]] bool initialized() const noexcept;
  [[nodiscard]] bool owns_device_objects() const noexcept;
  [[nodiscard]] bool has_font(std::string_view key) const noexcept;
  [[nodiscard]] bool has_image(std::string_view key) const noexcept;
  [[nodiscard]] bool has_shader(std::string_view key) const noexcept;
  [[nodiscard]] bool has_texture(std::string_view key) const noexcept;
  [[nodiscard]] D3D_FEATURE_LEVEL feature_level() const noexcept;
  [[nodiscard]] HWND window_handle() const noexcept;
  [[nodiscard]] ID3D11Device* device_handle() const noexcept;
  [[nodiscard]] ID3D11DeviceContext* device_context_handle() const noexcept;
  [[nodiscard]] IDXGISwapChain* swap_chain_handle() const noexcept;

 private:
  class Impl;
  Status create_factories();
  Status create_pipeline();
  Status realize_registered_fonts();
  Status realize_registered_shaders();
  void prepare_for_resize() noexcept;
  void release_transient_storage() noexcept;
  void reset_device_objects(bool clear_textures) noexcept;
  Status rebuild_render_targets();

  Dx11BackendConfig config_{};
  ExtentU viewport_{};
  std::size_t last_widget_count_{};
  D3D_FEATURE_LEVEL feature_level_{D3D_FEATURE_LEVEL_11_0};
  bool initialized_{false};
  bool owns_device_objects_{false};
  std::unique_ptr<Impl> impl_;
};

}  // namespace igr::backends
