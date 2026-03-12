#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>

#include "igr/react/runtime_bridge.hpp"
#include "menu_support.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

const igr::WidgetNode* find_widget_by_key(const igr::WidgetNode& node, std::string_view key) {
  if (node.key == key) {
    return &node;
  }
  for (const auto& child : node.children) {
    if (const igr::WidgetNode* match = find_widget_by_key(child, key); match != nullptr) {
      return match;
    }
  }
  return nullptr;
}

const igr::WidgetNode* find_widget_by_key(const igr::FrameDocument& document, std::string_view key) {
  for (const auto& root : document.roots) {
    if (const igr::WidgetNode* match = find_widget_by_key(root, key); match != nullptr) {
      return match;
    }
  }
  return nullptr;
}

struct RecordingRegistry final : igr::IResourceRegistry {
  std::unordered_set<std::string> fonts;
  std::unordered_set<std::string> images;
  std::unordered_set<std::string> shaders;

  igr::Status register_font(std::string_view key, const igr::FontResourceDesc&) override {
    fonts.emplace(key);
    return igr::Status::success();
  }

  void unregister_font(std::string_view key) noexcept override {
    fonts.erase(std::string(key));
  }

  igr::Status register_image(std::string_view key, const igr::ImageResourceDesc&) override {
    images.emplace(key);
    return igr::Status::success();
  }

  void unregister_image(std::string_view key) noexcept override {
    images.erase(std::string(key));
  }

  igr::Status register_shader(std::string_view key, const igr::ShaderResourceDesc&) override {
    shaders.emplace(key);
    return igr::Status::success();
  }

  void unregister_shader(std::string_view key) noexcept override {
    shaders.erase(std::string(key));
  }
};

}  // namespace

int main() {
  const bool live_bundle_available = std::filesystem::exists(igr::tests::menu::menu_bundle_path());
  std::unique_ptr<igr::react::ITransportRuntime> runtime = igr::tests::menu::make_menu_runtime(false);
  if (!runtime) {
    return fail("menu runtime tests could not find a Hermes bundle or menu fixture");
  }

  igr::react::RuntimeDocumentBridge bridge(std::move(runtime), {.retain_last_envelope = true});
  igr::Status status = bridge.initialize();
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("menu runtime bridge initialization failed");
  }

  RecordingRegistry registry;
  igr::tests::menu::MenuUiState state;
  igr::FrameDocument document;
  const igr::FrameInfo base_frame{
      .frame_index = 21,
      .viewport = {1280, 720},
      .delta_seconds = 1.0 / 60.0,
  };

  status = bridge.render_frame({
                                   .frame = base_frame,
                                   .state_json = igr::tests::menu::serialize_menu_state_json(state),
                               },
                               &document,
                               &registry);
  if (!status) {
    std::cerr << status.message() << '\n';
    bridge.shutdown();
    return fail("menu runtime bridge failed to render the initial menu payload");
  }

  if (bridge.last_envelope().session.name != "react-test-menu" || registry.fonts.size() != 2 || registry.images.size() != 1 ||
      registry.shaders.size() != 2 || find_widget_by_key(document, "action-apply") == nullptr ||
      find_widget_by_key(document, "menu-window") == nullptr) {
    bridge.shutdown();
    return fail("menu runtime bridge did not materialize the expected resources or root widgets");
  }

  if (live_bundle_available) {
    igr::tests::menu::apply_menu_widget_click("tab-render", &state);
    status = bridge.render_frame({
                                     .frame = {
                                         .frame_index = 22,
                                         .viewport = base_frame.viewport,
                                         .delta_seconds = base_frame.delta_seconds,
                                     },
                                     .state_json = igr::tests::menu::serialize_menu_state_json(state),
                                 },
                                 &document,
                                 &registry);
    if (!status) {
      std::cerr << status.message() << '\n';
      bridge.shutdown();
      return fail("menu runtime bridge failed to render the stateful render-tab payload");
    }

    if (find_widget_by_key(document, "render-preview") == nullptr || find_widget_by_key(document, "panel-overview") != nullptr) {
      bridge.shutdown();
      return fail("menu runtime bridge did not switch to the render panel after the native state update");
    }
    if (!bridge.last_envelope().fonts.empty() || !bridge.last_envelope().images.empty() || !bridge.last_envelope().shaders.empty() ||
        registry.fonts.size() != 2 || registry.images.size() != 1 || registry.shaders.size() != 2) {
      bridge.shutdown();
      return fail("menu runtime bridge did not retain previously registered resources across Hermes frames");
    }

    igr::tests::menu::apply_menu_widget_click("toggle-stats", &state);
    status = bridge.render_frame({
                                     .frame = {
                                         .frame_index = 23,
                                         .viewport = base_frame.viewport,
                                         .delta_seconds = base_frame.delta_seconds,
                                     },
                                     .state_json = igr::tests::menu::serialize_menu_state_json(state),
                                 },
                                 &document,
                                 &registry);
    if (!status) {
      std::cerr << status.message() << '\n';
      bridge.shutdown();
      return fail("menu runtime bridge failed to render the hidden-stats payload");
    }

    if (find_widget_by_key(document, "menu-stats-window") != nullptr) {
      bridge.shutdown();
      return fail("menu runtime bridge did not hide the diagnostics window after the toggle action");
    }
  } else {
    std::cout << "Menu Hermes bundle is unavailable in this environment; skipping stateful Hermes menu validation" << '\n';
  }

  bridge.shutdown();
  std::cout << "igr_menu_runtime_tests passed" << '\n';
  return 0;
}
