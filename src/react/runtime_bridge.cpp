#include "igr/react/runtime_bridge.hpp"

#include <algorithm>
#include <utility>

namespace igr::react {
namespace {

void release_transport_resources(const TransportAppliedResources& resources, IResourceRegistry& registry) noexcept {
  for (const auto& shader : resources.shaders) {
    registry.unregister_shader(shader.key);
  }
  for (const auto& image : resources.images) {
    registry.unregister_image(image.key);
  }
  for (const auto& font : resources.fonts) {
    registry.unregister_font(font.key);
  }
}

TransportEnvelope make_resource_envelope(const TransportAppliedResources& resources,
                                     TransportResourceMode resource_mode = TransportResourceMode::replace) {
  TransportEnvelope envelope;
  envelope.resource_mode = resource_mode;
  envelope.fonts = resources.fonts;
  envelope.images = resources.images;
  envelope.shaders = resources.shaders;
  return envelope;
}

template <typename Resource>
void merge_transport_resource_list(std::vector<Resource>* target, const std::vector<Resource>& delta) {
  if (target == nullptr) {
    return;
  }
  for (const auto& resource : delta) {
    const auto existing = std::find_if(target->begin(), target->end(), [&](const Resource& candidate) {
      return candidate.key == resource.key;
    });
    if (existing != target->end()) {
      *existing = resource;
    } else {
      target->push_back(resource);
    }
  }
}

TransportAppliedResources capture_transport_resources(const TransportEnvelope& envelope) {
  TransportAppliedResources resources;
  resources.fonts = envelope.fonts;
  resources.images = envelope.images;
  resources.shaders = envelope.shaders;
  return resources;
}

TransportAppliedResources merge_transport_resources(const TransportAppliedResources& previous, const TransportEnvelope& envelope) {
  TransportAppliedResources merged = previous;
  merge_transport_resource_list(&merged.fonts, envelope.fonts);
  merge_transport_resource_list(&merged.images, envelope.images);
  merge_transport_resource_list(&merged.shaders, envelope.shaders);
  return merged;
}

}  // namespace

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

RuntimeDocumentBridge::RuntimeDocumentBridge(std::unique_ptr<ITransportRuntime> runtime, RuntimeDocumentBridgeConfig config)
    : config_(config), runtime_(std::move(runtime)) {}

Status RuntimeDocumentBridge::initialize() {
  if (!runtime_) {
    return Status::invalid_argument("RuntimeDocumentBridge requires a transport runtime.");
  }
  Status status = runtime_->initialize();
  initialized_ = static_cast<bool>(status);
  return status;
}

Status RuntimeDocumentBridge::render_frame(FrameInfo frame, FrameDocument* document, IResourceRegistry* resource_registry) {
  return render_frame(RuntimeFrameRequest{.frame = frame}, document, resource_registry);
}

Status RuntimeDocumentBridge::render_frame(const RuntimeFrameRequest& request, FrameDocument* document, IResourceRegistry* resource_registry) {
  if (document == nullptr) {
    return Status::invalid_argument("RuntimeDocumentBridge requires a valid output document.");
  }
  if (!initialized_ || !runtime_) {
    return Status::not_ready("RuntimeDocumentBridge must be initialized before render_frame.");
  }

  RuntimeFrameResponse response;
  Status status = runtime_->render_transport(request, &response);
  if (!status) {
    return status;
  }
  if (response.sequence != 0 && response.sequence != request.frame.frame_index) {
    return Status::invalid_argument("RuntimeDocumentBridge received an out-of-sequence transport response.");
  }

  std::string payload = std::move(response.payload);
  TransportEnvelope envelope;
  status = parse_transport_envelope(payload, &envelope);
  if (!status) {
    return status;
  }

  const bool retain_resources = envelope.resource_mode == TransportResourceMode::retain;
  const TransportAppliedResources next_resources =
      retain_resources && has_applied_resources_ ? merge_transport_resources(applied_resources_, envelope) : capture_transport_resources(envelope);

  if (resource_registry != nullptr) {
    const bool switching_registries = has_applied_resources_ && last_resource_registry_ != nullptr && last_resource_registry_ != resource_registry;
    if (switching_registries) {
      if (retain_resources && has_applied_resources_) {
        const TransportEnvelope merged_resources = make_resource_envelope(next_resources);
        status = apply_transport_resources(merged_resources, *resource_registry);
      } else {
        status = apply_transport_resources(envelope, *resource_registry);
      }
    } else if (has_applied_resources_) {
      if (retain_resources) {
        status = apply_transport_resources(envelope, *resource_registry);
      } else {
        const TransportEnvelope previous_resources = make_resource_envelope(applied_resources_);
        status = reconcile_transport_resources(previous_resources, envelope, *resource_registry);
      }
    } else {
      status = apply_transport_resources(envelope, *resource_registry);
    }
    if (!status) {
      return status;
    }

    if (switching_registries) {
      release_transport_resources(applied_resources_, *last_resource_registry_);
    }

    applied_resources_ = next_resources;
    has_applied_resources_ = true;
    last_resource_registry_ = resource_registry;
  }

  UiContext context;
  status = context.begin_frame(request.frame);
  if (!status) {
    return status;
  }

  status = materialize_transport_envelope(envelope, context.builder());
  if (!status) {
    return status;
  }

  if (config_.retain_last_envelope) {
    last_envelope_ = std::move(envelope);
  } else {
    last_envelope_ = {};
  }
  if (config_.retain_last_payload) {
    last_payload_ = std::move(payload);
  } else {
    last_payload_.clear();
  }

  *document = context.end_frame();
  return Status::success();
}

void RuntimeDocumentBridge::shutdown() noexcept {
  if (has_applied_resources_ && last_resource_registry_ != nullptr) {
    release_transport_resources(applied_resources_, *last_resource_registry_);
  }
  if (runtime_) {
    runtime_->shutdown();
  }
  initialized_ = false;
  applied_resources_ = {};
  last_envelope_ = {};
  last_payload_.clear();
  has_applied_resources_ = false;
  last_resource_registry_ = nullptr;
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
