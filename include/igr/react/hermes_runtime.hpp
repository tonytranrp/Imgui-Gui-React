#pragma once

#include <memory>
#include <string>

#include "igr/react/runtime_bridge.hpp"

namespace igr::react {

struct HermesBundleConfig {
  std::string bundle_path;
  std::string bytecode_path;
  std::string entrypoint{"__igrRenderTransport"};
  bool prefer_bytecode{true};
};

struct HermesRuntimeConfig {
  HermesBundleConfig bundle{};
  bool enable_inspector{false};
};

class HermesTransportRuntime final : public ITransportRuntime {
 public:
  struct Impl;

  explicit HermesTransportRuntime(HermesRuntimeConfig config = {});
  ~HermesTransportRuntime() override;
  HermesTransportRuntime(const HermesTransportRuntime&) = delete;
  HermesTransportRuntime& operator=(const HermesTransportRuntime&) = delete;
  HermesTransportRuntime(HermesTransportRuntime&&) noexcept;
  HermesTransportRuntime& operator=(HermesTransportRuntime&&) noexcept;

  Status initialize() override;
  Status render_transport(const RuntimeFrameRequest& request, RuntimeFrameResponse* response) override;
  void shutdown() noexcept override;

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] const HermesRuntimeConfig& config() const noexcept;

 private:
  HermesRuntimeConfig config_{};
  std::unique_ptr<Impl> impl_;
  bool initialized_{false};
};

}  // namespace igr::react
