#include "igr/diagnostics.hpp"

#include <Windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

namespace igr {

ProcessMemorySnapshot query_process_memory() noexcept {
  ProcessMemorySnapshot snapshot{};
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) == 0) {
    return snapshot;
  }

  snapshot.working_set_bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
  snapshot.peak_working_set_bytes = static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
  snapshot.private_bytes = static_cast<std::uint64_t>(counters.PrivateUsage);
  snapshot.pagefile_bytes = static_cast<std::uint64_t>(counters.PagefileUsage);
  return snapshot;
}

GpuMemorySnapshot query_gpu_memory(IDXGIAdapter* adapter) noexcept {
  GpuMemorySnapshot snapshot{};
  if (adapter == nullptr) {
    return snapshot;
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
  if (FAILED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
    return snapshot;
  }

  DXGI_QUERY_VIDEO_MEMORY_INFO local_info{};
  DXGI_QUERY_VIDEO_MEMORY_INFO non_local_info{};
  if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local_info))) {
    return snapshot;
  }
  if (FAILED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non_local_info))) {
    return snapshot;
  }

  snapshot.available = true;
  snapshot.local_usage_bytes = local_info.CurrentUsage;
  snapshot.local_budget_bytes = local_info.Budget;
  snapshot.non_local_usage_bytes = non_local_info.CurrentUsage;
  snapshot.non_local_budget_bytes = non_local_info.Budget;
  return snapshot;
}

}  // namespace igr
