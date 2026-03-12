#pragma once

#include <string_view>

#include "igr/diagnostics.hpp"
#include "igr/frame.hpp"
#include "igr/geometry.hpp"
#include "igr/host.hpp"
#include "igr/result.hpp"

namespace igr {

enum class BackendKind {
  dx11,
  dx12,
};

struct BackendFrameStats {
  std::uint64_t frame_index{};
  std::size_t widget_count{};
  std::size_t quad_count{};
  std::size_t label_count{};
  std::size_t draw_batch_count{};
  std::size_t textured_batch_count{};
  std::size_t shader_batch_count{};
  std::size_t custom_draw_count{};
  std::size_t clip_rect_count{};
  std::size_t vertex_count{};
  std::uint64_t scene_build_microseconds{};
  std::uint64_t render_submit_microseconds{};
};

struct BackendTelemetrySnapshot {
  BackendFrameStats frame{};
  ProcessMemorySnapshot process_memory{};
  GpuMemorySnapshot gpu_memory{};
  ResourceUsageSnapshot resources{};
  std::vector<ScopeTelemetry> scopes;
};

struct BackendCapabilities {
  bool debug_layer{};
  bool user_textures{};
  bool user_shaders{};
  bool docking{};
  bool multi_viewport{};
  bool manual_host_binding{};
  bool host_state_restore{};
  bool injected_overlay{};
};

class IRendererBackend {
 public:
  virtual ~IRendererBackend() = default;

  [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;
  [[nodiscard]] virtual BackendCapabilities capabilities() const noexcept = 0;

  virtual Status initialize() = 0;
  virtual void invalidate_back_buffer_resources() noexcept = 0;
  virtual Status resize(ExtentU viewport) = 0;
  virtual Status render(const FrameDocument& document) = 0;
  virtual Status present() = 0;
  [[nodiscard]] virtual BackendFrameStats frame_stats() const noexcept = 0;
  [[nodiscard]] virtual BackendTelemetrySnapshot telemetry() const noexcept = 0;
  virtual void shutdown() noexcept = 0;
};

}  // namespace igr
