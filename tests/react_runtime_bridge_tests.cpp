#include <filesystem>
#include <iostream>
#include <memory>

#include "igr/react/hermes_runtime.hpp"
#include "igr/react/runtime_bridge.hpp"
#include "react_native_fixture.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

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

  igr::react::RuntimeDocumentBridge static_bridge(std::make_unique<igr::react::StaticTransportRuntime>(fixture_payload));
  status = static_bridge.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("RuntimeDocumentBridge initialization failed");
  }

  igr::FrameDocument runtime_document;
  status = static_bridge.render_frame(frame_info, &runtime_document);
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("RuntimeDocumentBridge failed to materialize the transport payload");
  }

  if (!static_bridge.initialized() || static_bridge.last_envelope().session.name != "react-native-test-physics" ||
      runtime_document.widget_count() != expected_document.widget_count()) {
    static_bridge.shutdown();
    return fail("RuntimeDocumentBridge did not preserve the expected transport/runtime state");
  }
  static_bridge.shutdown();

#if IGR_ENABLE_HERMES
  const std::filesystem::path bundle_path = igr::tests::react_native_bundle_path();
  if (!std::filesystem::exists(bundle_path)) {
    std::cerr << "Missing Hermes bundle: " << bundle_path.string() << '\n';
    return fail("Hermes bundle artifact is required for the runtime bridge test");
  }

  igr::react::RuntimeDocumentBridge hermes_bridge(std::make_unique<igr::react::HermesTransportRuntime>(igr::react::HermesRuntimeConfig{
      .bundle = {
          .bundle_path = bundle_path.string(),
          .bytecode_path = igr::tests::react_native_bytecode_path().string(),
          .entrypoint = "__igrRenderTransport",
          .prefer_bytecode = false,
      },
      .enable_inspector = false,
  }));

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
