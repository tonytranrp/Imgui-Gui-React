#pragma once

#include <concepts>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "igr/backends/dx11_backend.hpp"
#include "igr/backends/dx12_backend.hpp"
#include "igr/context.hpp"
#if IGR_ENABLE_HERMES
#include "igr/react/hermes_runtime.hpp"
#include "igr/react/runtime_bridge.hpp"
#endif
#include "igr/react/transport.hpp"

namespace igr::tests {
namespace {

template <typename Backend>
Status register_transport_shaders_if_supported(const react::TransportEnvelope& envelope, Backend& backend) {
  if constexpr (requires(Backend& candidate, const std::string& key, const ShaderResourceDesc& descriptor) {
                  { candidate.register_shader(key, descriptor) } -> std::same_as<Status>;
                }) {
    for (const auto& shader : envelope.shaders) {
      Status status = backend.register_shader(shader.key, shader.descriptor);
      if (!status) {
        return status;
      }
    }
  }
  return Status::success();
}

}  // namespace

inline std::filesystem::path react_native_fixture_path() {
  return std::filesystem::path(IGR_PROJECT_SOURCE_DIR) / "tests" / "fixtures" / "react-native-test.physics.json";
}

inline std::filesystem::path react_native_bundle_path() {
  return std::filesystem::path(IGR_PROJECT_SOURCE_DIR) / "artifacts" / "hermes" / "react-native-test.bundle.js";
}

inline std::filesystem::path react_native_bytecode_path() {
  return std::filesystem::path(IGR_PROJECT_SOURCE_DIR) / "artifacts" / "hermes" / "react-native-test.bundle.hbc";
}

inline Status read_text_file(const std::filesystem::path& path, std::string* output) {
  if (output == nullptr) {
    return Status::invalid_argument("read_text_file requires a valid output string.");
  }

  std::ifstream stream(path, std::ios::in | std::ios::binary);
  if (!stream.is_open()) {
    return Status::invalid_argument("Failed to open fixture file: " + path.string());
  }

  output->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return Status::success();
}

inline Status materialize_transport_document(const react::TransportEnvelope& envelope, FrameInfo info, FrameDocument* document) {
  if (document == nullptr) {
    return Status::invalid_argument("materialize_transport_document requires a valid output document.");
  }

  UiContext context;
  Status status = context.begin_frame(info);
  if (!status) {
    return status;
  }

  status = react::materialize_transport_envelope(envelope, context.builder());
  if (!status) {
    return status;
  }

  *document = context.end_frame();
  return Status::success();
}

inline Status load_react_native_fixture(FrameInfo info, react::TransportEnvelope* envelope, FrameDocument* document, std::string* payload = nullptr) {
  if (envelope == nullptr || document == nullptr) {
    return Status::invalid_argument("load_react_native_fixture requires valid output pointers.");
  }

  std::string local_payload;
  Status status = read_text_file(react_native_fixture_path(), &local_payload);
  if (!status) {
    return status;
  }

  status = react::parse_transport_envelope(local_payload, envelope);
  if (!status) {
    return status;
  }

  status = materialize_transport_document(*envelope, info, document);
  if (!status) {
    return status;
  }

  if (payload != nullptr) {
    *payload = std::move(local_payload);
  }

  return Status::success();
}

inline Status load_react_native_scene(FrameInfo info, react::TransportEnvelope* envelope, FrameDocument* document, std::string* payload = nullptr) {
#if IGR_ENABLE_HERMES
  if (std::filesystem::exists(react_native_bundle_path())) {
    react::RuntimeDocumentBridge bridge(std::make_unique<react::HermesTransportRuntime>(react::HermesRuntimeConfig{
        .bundle = {
            .bundle_path = react_native_bundle_path().string(),
            .bytecode_path = react_native_bytecode_path().string(),
            .entrypoint = "__igrRenderTransport",
            .prefer_bytecode = false,
        },
        .enable_inspector = false,
    }));

    Status status = bridge.initialize();
    if (!status) {
      return status;
    }

    status = bridge.render_frame(info, document);
    if (!status) {
      bridge.shutdown();
      return status;
    }

    if (envelope != nullptr) {
      *envelope = bridge.last_envelope();
    }
    if (payload != nullptr) {
      *payload = bridge.last_payload();
    }

    bridge.shutdown();
    return Status::success();
  }
#endif

  return load_react_native_fixture(info, envelope, document, payload);
}

inline Status register_transport_resources(const react::TransportEnvelope& envelope, backends::Dx11Backend& backend) {
  for (const auto& font : envelope.fonts) {
    Status status = backend.register_font(font.key, font.descriptor);
    if (!status) {
      return status;
    }
  }
  for (const auto& image : envelope.images) {
    Status status = backend.register_image(image.key, image.descriptor);
    if (!status) {
      return status;
    }
  }
  return register_transport_shaders_if_supported(envelope, backend);
}

inline Status register_transport_resources(const react::TransportEnvelope& envelope, backends::Dx12Backend& backend) {
  for (const auto& font : envelope.fonts) {
    Status status = backend.register_font(font.key, font.descriptor);
    if (!status) {
      return status;
    }
  }
  for (const auto& image : envelope.images) {
    Status status = backend.register_image(image.key, image.descriptor);
    if (!status) {
      return status;
    }
  }
  return register_transport_shaders_if_supported(envelope, backend);
}

}  // namespace igr::tests
