#include "igr/react/runtime_bridge.hpp"

#include <utility>

namespace igr::react {

StaticTransportRuntime::StaticTransportRuntime(std::string payload) : payload_(std::move(payload)) {}

StaticTransportRuntime::StaticTransportRuntime(Callback callback) : callback_(std::move(callback)) {}

Status StaticTransportRuntime::initialize() {
  initialized_ = true;
  return Status::success();
}

Status StaticTransportRuntime::render_transport(const RuntimeFrameRequest& request, RuntimeFrameResponse* response) {
  if (response == nullptr) {
    return Status::invalid_argument("StaticTransportRuntime requires a valid response output.");
  }
  if (!initialized_) {
    return Status::not_ready("StaticTransportRuntime must be initialized before render_transport.");
  }

  response->sequence = request.frame.frame_index;
  if (callback_) {
    return callback_(request, response);
  }

  if (payload_.empty()) {
    return Status::invalid_argument("StaticTransportRuntime has no payload or callback configured.");
  }

  response->payload = payload_;
  return Status::success();
}

void StaticTransportRuntime::shutdown() noexcept {
  initialized_ = false;
}

RuntimeDocumentBridge::RuntimeDocumentBridge(std::unique_ptr<ITransportRuntime> runtime) : runtime_(std::move(runtime)) {}

Status RuntimeDocumentBridge::initialize() {
  if (!runtime_) {
    return Status::invalid_argument("RuntimeDocumentBridge requires a transport runtime.");
  }
  Status status = runtime_->initialize();
  initialized_ = static_cast<bool>(status);
  return status;
}

Status RuntimeDocumentBridge::render_frame(FrameInfo frame, FrameDocument* document) {
  if (document == nullptr) {
    return Status::invalid_argument("RuntimeDocumentBridge requires a valid output document.");
  }
  if (!initialized_ || !runtime_) {
    return Status::not_ready("RuntimeDocumentBridge must be initialized before render_frame.");
  }

  RuntimeFrameResponse response;
  Status status = runtime_->render_transport({.frame = frame}, &response);
  if (!status) {
    return status;
  }

  last_payload_ = std::move(response.payload);
  status = parse_transport_envelope(last_payload_, &last_envelope_);
  if (!status) {
    return status;
  }

  UiContext context;
  status = context.begin_frame(frame);
  if (!status) {
    return status;
  }

  status = materialize_transport_envelope(last_envelope_, context.builder());
  if (!status) {
    return status;
  }

  *document = context.end_frame();
  return Status::success();
}

void RuntimeDocumentBridge::shutdown() noexcept {
  if (runtime_) {
    runtime_->shutdown();
  }
  initialized_ = false;
  last_envelope_ = {};
  last_payload_.clear();
}

bool RuntimeDocumentBridge::initialized() const noexcept {
  return initialized_;
}

const TransportEnvelope& RuntimeDocumentBridge::last_envelope() const noexcept {
  return last_envelope_;
}

const std::string& RuntimeDocumentBridge::last_payload() const noexcept {
  return last_payload_;
}

}  // namespace igr::react
