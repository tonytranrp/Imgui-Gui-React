#include <Windows.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <wrl/client.h>

#include "igr/backends/dx11_backend.hpp"
#include "igr/interaction.hpp"
#include "menu_support.hpp"

namespace {

using Microsoft::WRL::ComPtr;

struct AppState {
  igr::backends::Dx11Backend* backend{};
  igr::react::RuntimeDocumentBridge* bridge{};
  igr::tests::menu::MenuUiState* menu_state{};
  igr::FrameDocument document{};
  igr::InteractionState interaction{};
  igr::ExtentU viewport{1280, 720};
  std::optional<igr::ExtentU> pending_resize{};
  bool minimized{};
  std::string fatal_error;
};

void log_debug(std::string_view message) {
  const std::string line(message);
  OutputDebugStringA(line.c_str());
  OutputDebugStringA("\n");
}

void show_error_box(HWND owner, std::string_view title, const std::string& message) {
  log_debug(message);
  const std::string title_text(title);
  MessageBoxA(owner, message.c_str(), title_text.c_str(), MB_OK | MB_ICONERROR);
}

std::optional<std::uint64_t> parse_frame_limit(int argc, char** argv) {
  const auto parse_value = [](std::string_view token) -> std::optional<std::uint64_t> {
    std::uint64_t value = 0;
    const auto result = std::from_chars(token.data(), token.data() + token.size(), value);
    if (result.ec != std::errc() || result.ptr != token.data() + token.size()) {
      return std::nullopt;
    }
    return value;
  };

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--frames" && index + 1 < argc) {
      return parse_value(argv[index + 1]);
    }

    if (argument.starts_with("--frames=")) {
      return parse_value(argument.substr(9));
    }
  }

  return std::nullopt;
}

bool has_flag(int argc, char** argv, std::string_view flag) {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == flag) {
      return true;
    }
  }
  return false;
}

ComPtr<ID3D11ShaderResourceView> create_menu_texture(ID3D11Device* device) {
  if (device == nullptr) {
    return {};
  }

  static constexpr std::array<std::uint32_t, 16> kPixels{
      0xFF1E1210u, 0xFF2E2539u, 0xFF3C4870u, 0xFF5EA8D5u,
      0xFF161E2Fu, 0xFF253C56u, 0xFF38739Bu, 0xFF61C4FFu,
      0xFF10222Du, 0xFF1B3F4Bu, 0xFF2B7666u, 0xFF6EE3B9u,
      0xFF121018u, 0xFF2B1420u, 0xFF5B2338u, 0xFFF36D74u,
  };

  D3D11_TEXTURE2D_DESC texture_desc{};
  texture_desc.Width = 4;
  texture_desc.Height = 4;
  texture_desc.MipLevels = 1;
  texture_desc.ArraySize = 1;
  texture_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  texture_desc.SampleDesc.Count = 1;
  texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
  texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA initial_data{};
  initial_data.pSysMem = kPixels.data();
  initial_data.SysMemPitch = 4 * sizeof(std::uint32_t);

  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(device->CreateTexture2D(&texture_desc, &initial_data, &texture))) {
    return {};
  }

  ComPtr<ID3D11ShaderResourceView> shader_resource_view;
  if (FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &shader_resource_view))) {
    return {};
  }

  return shader_resource_view;
}

igr::PointerInputState sample_pointer_state(HWND window_handle) {
  POINT cursor{};
  GetCursorPos(&cursor);
  ScreenToClient(window_handle, &cursor);

  return {
      .position = {static_cast<float>(cursor.x), static_cast<float>(cursor.y)},
      .primary_down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
      .keyboard_requested = GetFocus() == window_handle,
  };
}

LRESULT CALLBACK window_proc(HWND window_handle, UINT message, WPARAM w_param, LPARAM l_param) {
  auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window_handle, GWLP_USERDATA));

  switch (message) {
    case WM_SIZE:
      if (state != nullptr) {
        state->viewport = {
            static_cast<std::uint32_t>(LOWORD(l_param)),
            static_cast<std::uint32_t>(HIWORD(l_param)),
        };
        state->minimized = (w_param == SIZE_MINIMIZED || state->viewport.width == 0 || state->viewport.height == 0);
        state->pending_resize = state->minimized ? std::nullopt : std::optional<igr::ExtentU>(state->viewport);
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(window_handle);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(window_handle, message, w_param, l_param);
  }
}

}  // namespace

int run_application(int argc, char** argv) {
  const std::optional<std::uint64_t> frame_limit = parse_frame_limit(argc, argv);
  const bool prefer_bytecode_runtime = !has_flag(argc, argv, "--source-runtime");
  const bool enable_debug_layer = has_flag(argc, argv, "--debug-layer");
  const bool disable_vsync = has_flag(argc, argv, "--no-vsync");
  const bool force_vsync = has_flag(argc, argv, "--vsync");
  const bool enable_vsync = force_vsync ? true : (disable_vsync ? false : !frame_limit.has_value());

  if (!std::filesystem::exists(igr::tests::menu::menu_bundle_path())) {
    show_error_box(nullptr,
                   "IGR React Menu Harness",
                   "The live Hermes menu bundle is missing. Run `npm run build`, `npm run export:fixtures`, and `npm run hermes:bundle` first.");
    return 1;
  }

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t* class_name = L"IGRReactMenuHarnessWindow";

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpfnWndProc = window_proc;
  window_class.lpszClassName = class_name;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));

  if (RegisterClassExW(&window_class) == 0) {
    show_error_box(nullptr, "IGR React Menu Harness", "Failed to register the menu harness window class.");
    return 1;
  }

  HWND window_handle = CreateWindowExW(
      0,
      class_name,
      L"IGR React Menu Harness",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      1280,
      720,
      nullptr,
      nullptr,
      instance,
      nullptr);

  if (window_handle == nullptr) {
    show_error_box(nullptr, "IGR React Menu Harness", "Failed to create the menu harness window.");
    UnregisterClassW(class_name, instance);
    return 1;
  }

  igr::backends::Dx11Theme theme;
  theme.window_background = {0.07f, 0.08f, 0.11f, 0.97f};
  theme.title_bar = {0.11f, 0.42f, 0.74f, 1.0f};
  theme.panel_background = {0.05f, 0.06f, 0.08f, 0.95f};
  theme.text_primary = {0.96f, 0.97f, 1.0f, 1.0f};
  theme.text_secondary = {0.72f, 0.79f, 0.89f, 1.0f};
  theme.button_background = {0.18f, 0.34f, 0.69f, 0.98f};
  theme.button_highlight = {0.24f, 0.44f, 0.86f, 1.0f};
  theme.progress_fill = {0.20f, 0.78f, 0.61f, 1.0f};
  theme.checkbox_fill = {0.24f, 0.67f, 0.98f, 1.0f};
  theme.body_text_size = 15.0f;
  theme.title_text_size = 16.0f;

  igr::backends::Dx11Backend backend({
      .diagnostics = {.enabled = false},
      .window_handle = window_handle,
      .initial_viewport = {1280, 720},
      .enable_debug_layer = enable_debug_layer,
      .enable_vsync = enable_vsync,
      .clear_color = {0.03f, 0.04f, 0.06f, 1.0f},
      .theme = theme,
  });

  igr::tests::menu::MenuUiState menu_state;
  std::unique_ptr<igr::react::ITransportRuntime> runtime = igr::tests::menu::make_menu_runtime(prefer_bytecode_runtime);
  if (!runtime) {
    show_error_box(window_handle, "IGR React Menu Harness", "Failed to create the menu runtime.");
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  igr::react::RuntimeDocumentBridge bridge(std::move(runtime));

  AppState app_state{
      .backend = &backend,
      .bridge = &bridge,
      .menu_state = &menu_state,
      .document = {},
      .interaction = {},
      .viewport = {1280, 720},
      .pending_resize = std::nullopt,
      .minimized = false,
      .fatal_error = {},
  };
  SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app_state));

  igr::Status status = backend.initialize();
  if (!status) {
    show_error_box(window_handle, "IGR React Menu Harness", "Dx11Backend initialization failed: " + status.message());
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  status = bridge.initialize();
  if (!status) {
    show_error_box(window_handle, "IGR React Menu Harness", "RuntimeDocumentBridge initialization failed: " + status.message());
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  const auto menu_texture = create_menu_texture(backend.device_handle());
  if (!menu_texture) {
    show_error_box(window_handle, "IGR React Menu Harness", "Failed to create the menu texture.");
    bridge.shutdown();
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  status = backend.register_texture("menu-gradient", menu_texture.Get());
  if (!status) {
    show_error_box(window_handle, "IGR React Menu Harness", "Menu texture registration failed: " + status.message());
    bridge.shutdown();
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  ShowWindow(window_handle, SW_SHOWDEFAULT);
  UpdateWindow(window_handle);

  log_debug("React menu harness running. bundle=" + igr::tests::menu::menu_bundle_path().string());

  using clock = std::chrono::steady_clock;
  const auto start_time = clock::now();
  auto previous_frame_time = start_time;
  std::uint64_t frame_index = 0;

  MSG message{};
  bool running = true;
  while (running) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        running = false;
        break;
      }

      TranslateMessage(&message);
      DispatchMessageW(&message);
    }

    if (!running) {
      break;
    }

    if (!app_state.fatal_error.empty()) {
      show_error_box(window_handle, "IGR React Menu Harness", app_state.fatal_error);
      running = false;
      break;
    }

    if (app_state.pending_resize.has_value()) {
      status = backend.resize(*app_state.pending_resize);
      if (!status) {
        show_error_box(window_handle, "IGR React Menu Harness", "Dx11Backend resize failed: " + status.message());
        running = false;
        break;
      }
      app_state.pending_resize.reset();
    }

    if (app_state.minimized) {
      Sleep(16);
      continue;
    }

    const auto now = clock::now();
    const double elapsed_seconds = std::chrono::duration<double>(now - start_time).count();
    const double delta_seconds = std::chrono::duration<double>(now - previous_frame_time).count();
    previous_frame_time = now;

    const igr::react::RuntimeFrameRequest request{
        .frame = {
            .frame_index = ++frame_index,
            .viewport = app_state.viewport,
            .delta_seconds = delta_seconds,
            .time_seconds = elapsed_seconds,
        },
        .state_json = igr::tests::menu::serialize_menu_state_json(menu_state),
    };

    status = bridge.render_frame(request, &app_state.document, &backend);
    if (!status) {
      show_error_box(window_handle, "IGR React Menu Harness", "RuntimeDocumentBridge render failed: " + status.message());
      running = false;
      break;
    }

    const igr::PointerInputState pointer_state = sample_pointer_state(window_handle);
    const igr::InteractionUpdate interaction_update =
        igr::update_interaction(app_state.document, {.input_mode = igr::InputMode::external_forwarded}, pointer_state, &app_state.interaction);

    if (interaction_update.clicked_widget_id != 0 &&
        igr::tests::menu::apply_menu_widget_click(app_state.document, interaction_update.clicked_widget_id, &menu_state)) {
      if (menu_state.request_close) {
        DestroyWindow(window_handle);
        continue;
      }

      status = bridge.render_frame({
                                       .frame = request.frame,
                                       .state_json = igr::tests::menu::serialize_menu_state_json(menu_state),
                                   },
                                   &app_state.document,
                                   &backend);
      if (!status) {
        show_error_box(window_handle, "IGR React Menu Harness", "RuntimeDocumentBridge re-render failed: " + status.message());
        running = false;
        break;
      }
    }

    status = backend.render(app_state.document);
    if (!status) {
      show_error_box(window_handle, "IGR React Menu Harness", "Dx11Backend render failed: " + status.message());
      running = false;
      break;
    }

    status = backend.present();
    if (!status) {
      show_error_box(window_handle, "IGR React Menu Harness", "Dx11Backend present failed: " + status.message());
      running = false;
      break;
    }

    if (frame_limit.has_value() && frame_index >= *frame_limit) {
      DestroyWindow(window_handle);
    }
  }

  bridge.shutdown();
  backend.shutdown();
  if (IsWindow(window_handle)) {
    DestroyWindow(window_handle);
  }
  UnregisterClassW(class_name, instance);
  return 0;
}

extern "C" int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return run_application(__argc, __argv);
}
