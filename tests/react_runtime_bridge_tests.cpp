#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

#include "igr/react/hermes_runtime.hpp"
#include "igr/react/runtime_bridge.hpp"
#include "react_native_fixture.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

struct RecordingRegistry final : igr::IResourceRegistry {
  std::vector<std::string> fonts;
  std::vector<std::string> images;
  std::vector<std::string> shaders;
  std::vector<std::string> removed_fonts;
  std::vector<std::string> removed_images;
  std::vector<std::string> removed_shaders;

  igr::Status register_font(std::string_view key, const igr::FontResourceDesc&) override {
    fonts.emplace_back(key);
    return igr::Status::success();
  }

  void unregister_font(std::string_view key) noexcept override {
    removed_fonts.emplace_back(key);
  }

  igr::Status register_image(std::string_view key, const igr::ImageResourceDesc&) override {
    images.emplace_back(key);
    return igr::Status::success();
  }

  void unregister_image(std::string_view key) noexcept override {
    removed_images.emplace_back(key);
  }

  igr::Status register_shader(std::string_view key, const igr::ShaderResourceDesc&) override {
    shaders.emplace_back(key);
    return igr::Status::success();
  }

  void unregister_shader(std::string_view key) noexcept override {
    removed_shaders.emplace_back(key);
  }
};

}  // namespace

int main() {
  const igr::FrameInfo frame_info{
      .frame_index = 33,
      .viewport = {1280, 720},
      .delta_seconds = 1.0 / 60.0,
  };

  igr::react::TransportEnvelope fixture_envelope;
  igr::FrameDocument expected_document;
  std::string fixture_payload;
  igr::Status status = igr::tests::load_react_native_fixture(frame_info, &fixture_envelope, &expected_document, &fixture_payload);
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("failed to load the TSX-generated runtime fixture");
  }

  igr::react::RuntimeDocumentBridge static_bridge(std::make_unique<igr::react::StaticTransportRuntime>(fixture_payload),
                                                  {.retain_last_envelope = true});
  RecordingRegistry resource_registry;
  status = static_bridge.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("RuntimeDocumentBridge initialization failed");
  }

  igr::FrameDocument runtime_document;
  status = static_bridge.render_frame(frame_info, &runtime_document, &resource_registry);
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("RuntimeDocumentBridge failed to materialize the transport payload");
  }

  if (!static_bridge.initialized() || static_bridge.last_envelope().session.name != "react-native-test-physics" ||
      runtime_document.widget_count() != expected_document.widget_count()) {
    static_bridge.shutdown();
    return fail("RuntimeDocumentBridge did not preserve the expected transport/runtime state");
  }
  if (resource_registry.fonts.size() != fixture_envelope.fonts.size() || resource_registry.images.size() != fixture_envelope.images.size() ||
      resource_registry.shaders.size() != fixture_envelope.shaders.size()) {
    static_bridge.shutdown();
    return fail("RuntimeDocumentBridge did not apply transport resources to the supplied registry");
  }
  static_bridge.shutdown();
  if (resource_registry.removed_fonts.size() != fixture_envelope.fonts.size() ||
      resource_registry.removed_images.size() != fixture_envelope.images.size() ||
      resource_registry.removed_shaders.size() != fixture_envelope.shaders.size()) {
    return fail("RuntimeDocumentBridge did not release applied transport resources during shutdown");
  }

  const std::string state_json = R"({"selectedPanel":"tools","buttonClicks":3})";
  igr::react::RuntimeDocumentBridge state_bridge(std::make_unique<igr::react::StaticTransportRuntime>(
      [fixture_payload, state_json](const igr::react::RuntimeFrameRequest& request, igr::react::RuntimeFrameResponse* response) {
        if (request.state_json != state_json) {
          return igr::Status::invalid_argument("RuntimeDocumentBridge did not forward the request state payload.");
        }
        response->sequence = request.frame.frame_index;
        response->payload = fixture_payload;
        return igr::Status::success();
      }));
  status = state_bridge.initialize();
  if (!status) {
    return fail("state runtime bridge initialization failed");
  }
  igr::FrameDocument state_document;
  status = state_bridge.render_frame({
                                          .frame = frame_info,
                                          .state_json = state_json,
                                      },
                                      &state_document);
  if (!status || state_document.widget_count() != expected_document.widget_count()) {
    if (!status) {
      std::cerr << status.message() << '\n';
    }
    state_bridge.shutdown();
    return fail("RuntimeDocumentBridge did not preserve the request state while materializing the payload");
  }
  state_bridge.shutdown();

  igr::react::RuntimeDocumentBridge out_of_sequence_bridge(std::make_unique<igr::react::StaticTransportRuntime>(
      [fixture_payload](const igr::react::RuntimeFrameRequest& request, igr::react::RuntimeFrameResponse* response) {
        response->sequence = request.frame.frame_index + 1;
        response->payload = fixture_payload;
        return igr::Status::success();
      }));
  status = out_of_sequence_bridge.initialize();
  if (!status) {
    return fail("out-of-sequence runtime bridge initialization failed");
  }
  igr::FrameDocument out_of_sequence_document;
  status = out_of_sequence_bridge.render_frame(frame_info, &out_of_sequence_document);
  out_of_sequence_bridge.shutdown();
  if (status) {
    return fail("RuntimeDocumentBridge accepted an out-of-sequence transport response");
  }

  igr::react::TransportEnvelope reduced_envelope = fixture_envelope;
  if (!reduced_envelope.fonts.empty()) {
    reduced_envelope.fonts.pop_back();
  }
  if (!reduced_envelope.images.empty()) {
    reduced_envelope.images.pop_back();
  }
  if (!reduced_envelope.shaders.empty()) {
    reduced_envelope.shaders.pop_back();
  }
  std::string reduced_payload;
  status = igr::react::serialize_transport_envelope(reduced_envelope, &reduced_payload);
  if (!status) {
    return fail("failed to serialize the reduced transport envelope");
  }

  igr::react::RuntimeDocumentBridge delta_bridge(std::make_unique<igr::react::StaticTransportRuntime>(
      [fixture_payload, reduced_payload](const igr::react::RuntimeFrameRequest& request, igr::react::RuntimeFrameResponse* response) {
        response->sequence = request.frame.frame_index;
        response->payload = request.frame.frame_index == 34 ? reduced_payload : fixture_payload;
        return igr::Status::success();
      }));
  RecordingRegistry delta_registry;
  status = delta_bridge.initialize();
  if (!status) {
    return fail("delta runtime bridge initialization failed");
  }
  igr::FrameDocument delta_document;
  status = delta_bridge.render_frame(frame_info, &delta_document, &delta_registry);
  if (!status) {
    delta_bridge.shutdown();
    return fail("delta runtime bridge failed on the initial resource application");
  }
  status = delta_bridge.render_frame({
                                       .frame_index = 34,
                                       .viewport = frame_info.viewport,
                                       .delta_seconds = frame_info.delta_seconds,
                                   },
                                   &delta_document,
                                   &delta_registry);
  if (!status) {
    delta_bridge.shutdown();
    return fail("delta runtime bridge failed on the reconciled resource application");
  }
  if ((fixture_envelope.fonts.size() > reduced_envelope.fonts.size() && delta_registry.removed_fonts.empty()) ||
      (fixture_envelope.images.size() > reduced_envelope.images.size() && delta_registry.removed_images.empty()) ||
      (fixture_envelope.shaders.size() > reduced_envelope.shaders.size() && delta_registry.removed_shaders.empty())) {
    delta_bridge.shutdown();
    return fail("RuntimeDocumentBridge did not unregister resources removed by a later transport payload");
  }
  delta_bridge.shutdown();

  igr::react::TransportEnvelope retained_envelope = fixture_envelope;
  retained_envelope.resource_mode = igr::react::TransportResourceMode::retain;
  std::string retained_payload;
  status = igr::react::serialize_transport_envelope(retained_envelope, &retained_payload);
  if (!status) {
    return fail("failed to serialize the retained transport envelope");
  }

  igr::react::TransportEnvelope retained_delta = fixture_envelope;
  retained_delta.resource_mode = igr::react::TransportResourceMode::retain;
  retained_delta.fonts.clear();
  retained_delta.images.clear();
  retained_delta.shaders.clear();
  std::string retained_delta_payload;
  status = igr::react::serialize_transport_envelope(retained_delta, &retained_delta_payload);
  if (!status) {
    return fail("failed to serialize the retained delta transport envelope");
  }

  igr::react::RuntimeDocumentBridge retained_bridge(std::make_unique<igr::react::StaticTransportRuntime>(
      [retained_payload, retained_delta_payload](const igr::react::RuntimeFrameRequest& request, igr::react::RuntimeFrameResponse* response) {
        response->sequence = request.frame.frame_index;
        response->payload = request.frame.frame_index == 35 ? retained_delta_payload : retained_payload;
        return igr::Status::success();
      }));
  RecordingRegistry retained_registry;
  status = retained_bridge.initialize();
  if (!status) {
    return fail("retained runtime bridge initialization failed");
  }
  igr::FrameDocument retained_document;
  status = retained_bridge.render_frame(frame_info, &retained_document, &retained_registry);
  if (!status) {
    retained_bridge.shutdown();
    return fail("retained runtime bridge failed on the initial resource application");
  }
  status = retained_bridge.render_frame({
                                          .frame_index = 35,
                                          .viewport = frame_info.viewport,
                                          .delta_seconds = frame_info.delta_seconds,
                                      },
                                      &retained_document,
                                      &retained_registry);
  if (!status) {
    retained_bridge.shutdown();
    return fail("retained runtime bridge failed on the retained resource update");
  }
  if (!retained_registry.removed_fonts.empty() || !retained_registry.removed_images.empty() || !retained_registry.removed_shaders.empty()) {
    retained_bridge.shutdown();
    return fail("RuntimeDocumentBridge should retain previously applied resources when the transport payload uses retain mode");
  }
  retained_bridge.shutdown();
  if (retained_registry.removed_fonts.size() != fixture_envelope.fonts.size() ||
      retained_registry.removed_images.size() != fixture_envelope.images.size() ||
      retained_registry.removed_shaders.size() != fixture_envelope.shaders.size()) {
    return fail("RuntimeDocumentBridge did not release retained resources during shutdown");
  }

#if IGR_ENABLE_HERMES
  const std::filesystem::path bundle_path = igr::tests::react_native_bundle_path();
  if (!std::filesystem::exists(bundle_path)) {
    std::cout << "Hermes bundle artifact is unavailable in this environment; skipping live Hermes runtime validation" << '\n';
    std::cout << "igr_react_runtime_bridge_tests passed" << '\n';
    return 0;
  }

  igr::react::RuntimeDocumentBridge hermes_bridge(
      std::make_unique<igr::react::HermesTransportRuntime>(igr::react::HermesRuntimeConfig{
          .bundle = {
              .bundle_path = bundle_path.string(),
              .bytecode_path = igr::tests::react_native_bytecode_path().string(),
              .entrypoint = "__igrRenderTransport",
              .prefer_bytecode = false,
          },
          .enable_inspector = false,
      }),
      {
          .retain_last_envelope = true,
          .retain_last_payload = true,
      });

  status = hermes_bridge.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("Hermes runtime bridge initialization failed");
  }

  igr::FrameDocument hermes_document;
  status = hermes_bridge.render_frame(frame_info, &hermes_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    hermes_bridge.shutdown();
    return fail("Hermes runtime bridge failed to materialize the runtime transport payload");
  }

  if (!hermes_bridge.initialized() || hermes_bridge.last_envelope().session.name != fixture_envelope.session.name ||
      hermes_bridge.last_envelope().session.host.host_mode != fixture_envelope.session.host.host_mode ||
      hermes_bridge.last_envelope().fonts.size() != fixture_envelope.fonts.size() ||
      hermes_bridge.last_envelope().images.size() != fixture_envelope.images.size() ||
      hermes_document.widget_count() != expected_document.widget_count() || hermes_document.info.frame_index != frame_info.frame_index) {
    hermes_bridge.shutdown();
    return fail("Hermes runtime bridge did not preserve the expected runtime state");
  }

  igr::react::TransportEnvelope parsed_runtime_envelope;
  status = igr::react::parse_transport_envelope(hermes_bridge.last_payload(), &parsed_runtime_envelope);
  if (!status) {
    std::cerr << status.message() << '\n';
    hermes_bridge.shutdown();
    return fail("Hermes runtime returned an invalid transport payload");
  }

  if (parsed_runtime_envelope.sequence != frame_info.frame_index || parsed_runtime_envelope.root.children.size() != fixture_envelope.root.children.size()) {
    hermes_bridge.shutdown();
    return fail("Hermes runtime transport payload did not match the expected scene contract");
  }

  hermes_bridge.shutdown();

  const std::filesystem::path bytecode_path = igr::tests::react_native_bytecode_path();
  if (std::filesystem::exists(bytecode_path)) {
    igr::react::RuntimeDocumentBridge hermes_bytecode_bridge(
        std::make_unique<igr::react::HermesTransportRuntime>(igr::react::HermesRuntimeConfig{
            .bundle = {
                .bundle_path = bundle_path.string(),
                .bytecode_path = bytecode_path.string(),
                .entrypoint = "__igrRenderTransport",
                .prefer_bytecode = true,
                .allow_source_fallback = false,
            },
            .enable_inspector = false,
            .enable_gc_api = true,
            .collect_garbage_after_initialize = true,
            .collect_garbage_every_n_renders = 8,
            .trim_working_set_after_gc = false,
        }),
        {
            .retain_last_envelope = true,
            .retain_last_payload = true,
        });

    status = hermes_bytecode_bridge.initialize();
    if (!status) {
      std::cerr << status.message() << '\n';
      return fail("Hermes bytecode runtime bridge initialization failed");
    }

    for (std::uint64_t index = 0; index < 32; ++index) {
      const igr::FrameInfo bytecode_frame{
          .frame_index = frame_info.frame_index + index,
          .viewport = frame_info.viewport,
          .delta_seconds = frame_info.delta_seconds,
          .time_seconds = static_cast<double>(index) * frame_info.delta_seconds,
      };
      status = hermes_bytecode_bridge.render_frame(bytecode_frame, &hermes_document);
      if (!status) {
        std::cerr << status.message() << '\n';
        hermes_bytecode_bridge.shutdown();
        return fail("Hermes bytecode runtime bridge failed during repeated renders");
      }
    }

    if (hermes_document.widget_count() != expected_document.widget_count() ||
        hermes_bytecode_bridge.last_envelope().session.name != fixture_envelope.session.name) {
      hermes_bytecode_bridge.shutdown();
      return fail("Hermes bytecode runtime bridge did not preserve the expected runtime state");
    }

    hermes_bytecode_bridge.shutdown();
  }
#else
  igr::react::HermesTransportRuntime hermes_runtime({
      .bundle = {
          .bundle_path = "artifacts/hermes/react-native-test.bundle.js",
          .bytecode_path = "artifacts/hermes/react-native-test.bundle.hbc",
          .entrypoint = "__igrRenderTransport",
          .prefer_bytecode = false,
      },
      .enable_inspector = true,
  });
  status = hermes_runtime.initialize();
  if (status) {
    return fail("HermesTransportRuntime should report unsupported until the packaged Hermes runtime is linked");
  }
#endif

  std::cout << "igr_react_runtime_bridge_tests passed" << '\n';
  return 0;
}
