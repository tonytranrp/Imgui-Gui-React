#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>

#include "igr/interaction.hpp"
#include "igr/react/runtime_bridge.hpp"
#if IGR_ENABLE_HERMES
#include "igr/react/hermes_runtime.hpp"
#endif

namespace igr::tests::menu {

inline constexpr std::string_view kMenuRuntimeEntrypoint = "__igrRenderMenuTransport";

struct MenuUiState {
  std::string selected_tab{"overview"};
  bool show_stats{true};
  bool glow_enabled{true};
  std::uint32_t accent_index{};
  std::uint32_t apply_count{};
  std::string last_action{"Ready"};
  bool request_close{false};
};

inline std::filesystem::path menu_fixture_path() {
  return std::filesystem::path(IGR_PROJECT_SOURCE_DIR) / "tests" / "menu" / "dist" / "react-menu.fixture.json";
}

inline std::filesystem::path tracked_menu_fixture_path() {
  return std::filesystem::path(IGR_PROJECT_SOURCE_DIR) / "tests" / "fixtures" / "react-menu-test.menu.json";
}

inline std::filesystem::path menu_bundle_path() {
  return std::filesystem::path(IGR_PROJECT_SOURCE_DIR) / "artifacts" / "hermes" / "react-menu-test.bundle.js";
}

inline std::filesystem::path menu_bytecode_path() {
  return std::filesystem::path(IGR_PROJECT_SOURCE_DIR) / "artifacts" / "hermes" / "react-menu-test.bundle.hbc";
}

inline Status read_text_file(const std::filesystem::path& path, std::string* output) {
  if (output == nullptr) {
    return Status::invalid_argument("menu test file loading requires a valid output string.");
  }

  std::ifstream stream(path, std::ios::in | std::ios::binary);
  if (!stream.is_open()) {
    return Status::invalid_argument("Failed to open menu test file: " + path.string());
  }

  output->assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
  return Status::success();
}

inline std::string escape_json(std::string_view value) {
  std::string output;
  output.reserve(value.size() + 8);
  for (const char character : value) {
    switch (character) {
      case '\\':
        output += "\\\\";
        break;
      case '"':
        output += "\\\"";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        output += character;
        break;
    }
  }
  return output;
}

inline std::string serialize_menu_state_json(const MenuUiState& state) {
  std::string payload;
  payload.reserve(192);
  payload += "{\"selectedTab\":\"";
  payload += escape_json(state.selected_tab);
  payload += "\",\"showStats\":";
  payload += state.show_stats ? "true" : "false";
  payload += ",\"glowEnabled\":";
  payload += state.glow_enabled ? "true" : "false";
  payload += ",\"accentIndex\":";
  payload += std::to_string(state.accent_index);
  payload += ",\"applyCount\":";
  payload += std::to_string(state.apply_count);
  payload += ",\"lastAction\":\"";
  payload += escape_json(state.last_action);
  payload += "\"}";
  return payload;
}

inline void reset_menu_state(MenuUiState* state) {
  if (state == nullptr) {
    return;
  }
  *state = MenuUiState{};
}

inline bool apply_menu_widget_click(std::string_view key, MenuUiState* state) {
  if (state == nullptr || key.empty()) {
    return false;
  }

  state->request_close = false;

  if (key == "tab-overview") {
    state->selected_tab = "overview";
    state->last_action = "Overview selected";
    return true;
  }
  if (key == "tab-render") {
    state->selected_tab = "render";
    state->last_action = "Render selected";
    return true;
  }
  if (key == "tab-react") {
    state->selected_tab = "react";
    state->last_action = "React selected";
    return true;
  }
  if (key == "toggle-stats") {
    state->show_stats = !state->show_stats;
    state->last_action = state->show_stats ? "Stats enabled" : "Stats hidden";
    return true;
  }
  if (key == "toggle-glow") {
    state->glow_enabled = !state->glow_enabled;
    state->last_action = state->glow_enabled ? "Glow enabled" : "Glow disabled";
    return true;
  }
  if (key == "accent-prev") {
    state->accent_index = (state->accent_index + 3u) % 4u;
    state->last_action = "Accent stepped back";
    return true;
  }
  if (key == "accent-next") {
    state->accent_index = (state->accent_index + 1u) % 4u;
    state->last_action = "Accent stepped forward";
    return true;
  }
  if (key == "action-apply") {
    state->apply_count += 1;
    state->last_action = "Changes applied";
    return true;
  }
  if (key == "action-reset") {
    reset_menu_state(state);
    state->last_action = "Menu reset";
    return true;
  }
  if (key == "action-close") {
    state->last_action = "Close requested";
    state->request_close = true;
    return true;
  }

  return false;
}

inline bool apply_menu_widget_click(const FrameDocument& document, WidgetId widget_id, MenuUiState* state) {
  return apply_menu_widget_click(find_widget_key(document, widget_id), state);
}

inline std::unique_ptr<react::ITransportRuntime> make_menu_runtime(bool prefer_bytecode = true) {
#if IGR_ENABLE_HERMES
  if (std::filesystem::exists(menu_bundle_path())) {
    return std::make_unique<react::HermesTransportRuntime>(react::HermesRuntimeConfig{
        .bundle = {
            .bundle_path = menu_bundle_path().string(),
            .bytecode_path = menu_bytecode_path().string(),
            .entrypoint = std::string(kMenuRuntimeEntrypoint),
            .prefer_bytecode = prefer_bytecode,
            .allow_source_fallback = !prefer_bytecode,
        },
        .enable_inspector = false,
        .enable_gc_api = true,
        .collect_garbage_after_initialize = true,
        .collect_garbage_every_n_renders = prefer_bytecode ? 15u : 0u,
        .trim_working_set_after_gc = false,
    });
  }
#endif

  std::string payload;
  const std::filesystem::path preferred_fixture_path =
      std::filesystem::exists(menu_fixture_path()) ? menu_fixture_path() : tracked_menu_fixture_path();
  if (const Status status = read_text_file(preferred_fixture_path, &payload); status) {
    return std::make_unique<react::StaticTransportRuntime>(std::move(payload));
  }
  return {};
}

}  // namespace igr::tests::menu
