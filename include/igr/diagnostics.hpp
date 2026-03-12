#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct IDXGIAdapter;

namespace igr {

struct DiagnosticsConfig {
  bool enabled{true};
  bool collect_process_memory{true};
  bool collect_gpu_memory{true};
  bool collect_scope_timings{true};
  bool collect_resource_usage{true};
  std::uint32_t memory_sample_interval{1};
  std::size_t top_scope_limit{8};
};

struct ResourceBudgetConfig {
  std::size_t max_cached_wide_strings{2048};
  std::size_t max_retained_scene_quads{8192};
  std::size_t max_retained_text_labels{4096};
  std::size_t max_retained_vertices{49152};
  std::size_t max_retained_batches{8192};
  std::size_t max_retained_shader_constants{1024};
  std::size_t max_text_atlas_dimension{2048};
  std::size_t text_atlas_padding{1};
};

struct ProcessMemorySnapshot {
  std::uint64_t working_set_bytes{};
  std::uint64_t peak_working_set_bytes{};
  std::uint64_t private_bytes{};
  std::uint64_t pagefile_bytes{};
};

struct GpuMemorySnapshot {
  bool available{};
  std::uint64_t local_usage_bytes{};
  std::uint64_t local_budget_bytes{};
  std::uint64_t non_local_usage_bytes{};
  std::uint64_t non_local_budget_bytes{};
};

struct ScopeTelemetry {
  std::string name;
  std::uint64_t call_count{};
  std::uint64_t total_microseconds{};
  std::uint64_t max_microseconds{};
};

struct ResourceUsageSnapshot {
  std::size_t font_count{};
  std::size_t image_count{};
  std::size_t shader_count{};
  std::size_t texture_count{};
  std::uint64_t font_cache_bytes{};
  std::uint64_t wide_text_cache_bytes{};
  std::uint64_t scene_bytes{};
  std::uint64_t scratch_vertex_bytes{};
  std::uint64_t scratch_batch_bytes{};
  std::uint64_t gpu_vertex_buffer_bytes{};
  std::uint64_t gpu_constant_buffer_bytes{};
  std::uint64_t gpu_text_atlas_bytes{};
  std::uint64_t cpu_text_bitmap_bytes{};
  std::uint64_t total_estimated_bytes{};
  bool text_interop_active{};
  bool text_atlas_active{};
};

[[nodiscard]] ProcessMemorySnapshot query_process_memory() noexcept;
[[nodiscard]] GpuMemorySnapshot query_gpu_memory(IDXGIAdapter* adapter) noexcept;

}  // namespace igr
