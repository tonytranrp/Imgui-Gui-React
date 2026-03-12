#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "igr/frame.hpp"
#include "igr/resources.hpp"
#include "igr/react/transport.hpp"
#include "igr/result.hpp"

namespace igr::react {

struct RuntimeFrameRequest {
  FrameInfo frame{};
  std::string state_json;
};

struct RuntimeFrameResponse {
  std::uint64_t sequence{};
  std::string payload;
};

class ITransportRuntime {
 public:
  virtual ~ITransportRuntime() = default;

  virtual Status initialize() = 0;
  virtual Status render_transport(const RuntimeFrameRequest& request, RuntimeFrameResponse* response) = 0;
  virtual void shutdown() noexcept = 0;
};

class StaticTransportRuntime final : public ITransportRuntime {
 public:
  using Callback = std::function<Status(const RuntimeFrameRequest&, RuntimeFrameResponse*)>;

  explicit StaticTransportRuntime(std::string payload);
  explicit StaticTransportRuntime(Callback callback);

  Status initialize() override;
  Status render_transport(const RuntimeFrameRequest& request, RuntimeFrameResponse* response) override;
  void shutdown() noexcept override;

 private:
  Callback callback_{};
  std::string payload_{};
  bool initialized_{false};
};

struct RuntimeDocumentBridgeConfig {
  bool retain_last_envelope{false};
  bool retain_last_payload{false};
};

struct TransportAppliedResources {
  std::vector<TransportFontResource> fonts;
  std::vector<TransportImageResource> images;
  std::vector<TransportShaderResource> shaders;
};

class RuntimeDocumentBridge final {
 public:
  explicit RuntimeDocumentBridge(std::unique_ptr<ITransportRuntime> runtime, RuntimeDocumentBridgeConfig config = {});

  Status initialize();
  Status render_frame(FrameInfo frame, FrameDocument* document, IResourceRegistry* resource_registry = nullptr);
  Status render_frame(const RuntimeFrameRequest& request, FrameDocument* document, IResourceRegistry* resource_registry = nullptr);
  void shutdown() noexcept;

  [[nodiscard]] bool initialized() const noexcept;
 [[nodiscard]] const TransportEnvelope& last_envelope() const noexcept;
  [[nodiscard]] const std::string& last_payload() const noexcept;

 private:
  RuntimeDocumentBridgeConfig config_{};
  std::unique_ptr<ITransportRuntime> runtime_;
  TransportAppliedResources applied_resources_{};
  TransportEnvelope last_envelope_{};
  std::string last_payload_{};
  bool initialized_{false};
  bool has_applied_resources_{false};
  IResourceRegistry* last_resource_registry_{};
};

}  // namespace igr::react
