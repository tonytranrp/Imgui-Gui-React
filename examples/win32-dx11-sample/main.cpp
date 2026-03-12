#include <Windows.h>

#include <array>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <wrl/client.h>

#include "igr/backends/dx11_backend.hpp"
#include "igr/context.hpp"
#include "igr/react/document.hpp"

namespace {

using Microsoft::WRL::ComPtr;

struct AppState {
  igr::backends::Dx11Backend* backend{};
  igr::ExtentU viewport{1280, 720};
  std::optional<igr::ExtentU> pending_resize{};
  bool minimized{false};
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

ComPtr<ID3D11ShaderResourceView> create_demo_texture(ID3D11Device* device) {
  if (device == nullptr) {
    return {};
  }

  static constexpr std::array<std::uint32_t, 16> kPixels{
      0xFF58361Fu, 0xFF825830u, 0xFFB68745u, 0xFFF6C863u,
      0xFF483413u, 0xFF684E23u, 0xFFA38236u, 0xFFC9A94Eu,
      0xFF342510u, 0xFF523D1Au, 0xFF7E662Au, 0xFFB1953Fu,
      0xFF1F1608u, 0xFF403114u, 0xFF654F20u, 0xFF917C32u,
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

igr::ShaderResourceDesc make_demo_shader() {
  igr::ShaderResourceDesc shader{};
  shader.pixel.language = igr::ShaderLanguage::hlsl;
  shader.pixel.entry_point = "main";
  shader.pixel.source = R"(
cbuffer IgrShaderConstants : register(b0) {
  float4 igrTint;
  float4 igrParam0;
  float4 igrParam1;
  float4 igrParam2;
  float4 igrParam3;
  float4 igrRect;
  float4 igrViewportAndTime;
  float4 igrFrameData;
};

struct PSInput {
  float4 position : SV_POSITION;
  float4 color : COLOR;
  float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target {
  const float wave = 0.55 + 0.45 * sin(igrViewportAndTime.z * 2.0 + input.uv.x * 6.28318);
  const float4 surface = float4(0.18 + input.uv.x * 0.22, 0.42 + input.uv.y * 0.34, wave, 1.0);
  return surface * igrTint * input.color;
}
)";
  shader.samples_texture = false;
  shader.blend_mode = igr::ShaderBlendMode::alpha;
  return shader;
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

std::string format_mebibytes(std::uint64_t bytes) {
  const double mib = static_cast<double>(bytes) / (1024.0 * 1024.0);
  char buffer[32]{};
  std::snprintf(buffer, sizeof(buffer), "%.1f MiB", mib);
  return buffer;
}

igr::FrameDocument build_demo_document(std::uint64_t frame_index,
                                       double elapsed_seconds,
                                       double delta_seconds,
                                       igr::ExtentU viewport,
                                       std::string_view preview_texture,
                                       std::string_view preview_resource,
                                       const igr::BackendTelemetrySnapshot* telemetry) {
  const float native_progress = static_cast<float>(std::fmod(elapsed_seconds * 0.25, 1.0));
  const float bridge_progress = static_cast<float>(std::fmod(elapsed_seconds * 0.18 + 0.33, 1.0));
  const bool validation_enabled = (frame_index / 90) % 2 == 0;

  igr::UiContext context;
  context.begin_frame({
      .frame_index = frame_index,
      .viewport = viewport,
      .delta_seconds = delta_seconds,
      .time_seconds = elapsed_seconds,
  });

  auto& builder = context.builder();
  builder.begin_window("runtime-native", "Native DX11 Window", {{32.0f, 32.0f}, {420.0f, 368.0f}});
  builder.begin_stack("native-layout", igr::Axis::vertical);
  builder.text("native-status", "DX11 backend is rendering live D3D11 primitives with DirectWrite text.", "body-md");
  builder.begin_clip_rect("native-preview-clip", {220.0f, 122.0f});
  builder.checkbox("native-validation", "Validation overlay enabled", validation_enabled);
  builder.progress_bar("native-progress", "Streaming geometry", native_progress);
  igr::ShaderUniformData native_shader_uniforms{};
  native_shader_uniforms.tint = {0.72f, 0.90f, 1.0f, 0.92f};
  native_shader_uniforms.params[0] = {static_cast<float>(frame_index), static_cast<float>(delta_seconds), static_cast<float>(viewport.width),
                                      static_cast<float>(viewport.height)};
  builder.shader_rect("native-shader-surface", "demo-shader", {{8.0f, 10.0f}, {180.0f, 28.0f}}, {}, {}, native_shader_uniforms);
  builder.image("native-image", preview_texture, {180.0f, 84.0f}, "DX11 user texture", "demo-card");
  builder.fill_rect("native-accent-fill", {{8.0f, 8.0f}, {26.0f, 10.0f}}, {0.25f, 0.70f, 0.98f, 0.85f});
  builder.draw_line("native-accent-line", {6.0f, 112.0f}, {204.0f, 112.0f}, {0.08f, 0.12f, 0.18f, 1.0f}, 2.0f);
  builder.end_container();
  builder.begin_stack("native-actions", igr::Axis::horizontal);
  builder.button("native-action", frame_index % 120 < 60 ? "Frame phase A" : "Frame phase B");
  builder.button("native-pulse", elapsed_seconds - static_cast<int>(elapsed_seconds) < 0.5 ? "Pulse low" : "Pulse high");
  builder.end_container();
  builder.separator("native-separator");
  builder.text("native-metrics", std::string("Viewport: ") + std::to_string(viewport.width) + " x " + std::to_string(viewport.height), "body-md");
  builder.end_container();
  builder.end_container();

  igr::react::ElementNode bridge_window{
      .type = "window",
      .key = "bridge-runtime",
      .props = {
          {"title", std::string("Declarative Bridge Window")},
          {"x", 500.0},
          {"y", 80.0},
          {"width", 420.0},
          {"height", 380.0},
      },
      .children = {
          {
              .type = "stack",
              .key = "bridge-layout",
              .props = {{"axis", std::string("vertical")}},
              .children = {
                  {.type = "text", .key = "bridge-text", .props = {{"value", std::string("React-style trees materialize into the native immediate-mode frame.")}, {"font", std::string("body-md")}}},
                  {
                      .type = "clip_rect",
                      .key = "bridge-clip",
                      .props = {{"width", 220.0}, {"height", 118.0}},
                      .children = {
                          {.type = "checkbox", .key = "bridge-checkbox", .props = {{"label", std::string("JSX bridge active")}, {"checked", true}}},
                          {.type = "shader_image",
                           .key = "bridge-shader-image",
                           .props = {{"shader", std::string("demo-shader")},
                                     {"texture", std::string(preview_texture)},
                                     {"resource", std::string(preview_resource)},
                                     {"width", 180.0},
                                     {"height", 84.0},
                                     {"label", std::string("React shader widget")},
                                     {"tint", std::string("#9CE0FFFF")},
                                     {"param0", std::string(std::to_string(frame_index) + ", " + std::to_string(delta_seconds) + ", " +
                                                            std::to_string(viewport.width) + ", " + std::to_string(viewport.height))}}},
                          {.type = "image", .key = "bridge-image", .props = {{"texture", std::string(preview_texture)}, {"resource", std::string("demo-card")}, {"width", 180.0}, {"height", 84.0}, {"label", std::string("React image widget")}}},
                          {.type = "fill_rect", .key = "bridge-fill", .props = {{"x", 8.0}, {"y", 8.0}, {"width", 24.0}, {"height", 10.0}, {"color", std::string("#2FA9F2D8")}}},
                          {.type = "line", .key = "bridge-line", .props = {{"x1", 6.0}, {"y1", 108.0}, {"x2", 204.0}, {"y2", 108.0}, {"thickness", 2.0}, {"color", std::string("#14202EFF")}}},
                      },
                  },
                  {.type = "progress_bar", .key = "bridge-progress", .props = {{"label", std::string("Bridge serialization")}, {"value", bridge_progress}}},
                  {.type = "separator", .key = "bridge-separator"},
                  {
                      .type = "stack",
                      .key = "bridge-actions",
                      .props = {{"axis", std::string("horizontal")}},
                      .children = {
                          {.type = "button", .key = "bridge-button-width", .props = {{"label", std::string("Viewport width")}, {"enabled", true}}},
                          {.type = "button", .key = "bridge-button-height", .props = {{"label", std::string("Viewport height")}, {"enabled", true}}},
                      },
                  },
                  {.type = "text", .key = "bridge-metric", .props = {{"value", std::string("Width: ") + std::to_string(viewport.width)}, {"font", std::string("body-md")}}},
              },
          },
      },
  };

  igr::react::materialize(bridge_window, builder);

  if (telemetry != nullptr) {
    builder.begin_window("runtime-diagnostics", "Runtime Telemetry", {{952.0f, 32.0f}, {292.0f, 242.0f}});
    builder.begin_stack("runtime-diagnostics-layout", igr::Axis::vertical);
    builder.text("diag-working-set", "Working set: " + format_mebibytes(telemetry->process_memory.working_set_bytes), "body-md");
    builder.text("diag-private", "Private bytes: " + format_mebibytes(telemetry->process_memory.private_bytes), "body-md");
    builder.text("diag-scene", "Scene scratch: " + format_mebibytes(telemetry->resources.scene_bytes + telemetry->resources.scratch_vertex_bytes +
                                                                     telemetry->resources.scratch_batch_bytes),
                 "body-md");
    builder.text("diag-cache", "Text cache: " + format_mebibytes(telemetry->resources.wide_text_cache_bytes), "body-md");
    if (telemetry->gpu_memory.available) {
      builder.text("diag-gpu", "GPU local: " + format_mebibytes(telemetry->gpu_memory.local_usage_bytes) + " / " +
                                    format_mebibytes(telemetry->gpu_memory.local_budget_bytes),
                   "body-md");
    }
    builder.separator("diag-separator");
    if (!telemetry->scopes.empty()) {
      const auto& scope = telemetry->scopes.front();
      builder.text("diag-top-scope", "Top scope: " + scope.name + " (" + std::to_string(scope.total_microseconds) + " us)", "body-md");
    }
    builder.text("diag-batches", "Batches: " + std::to_string(telemetry->frame.draw_batch_count), "body-md");
    builder.end_container();
    builder.end_container();
  }
  return context.end_frame();
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
  const bool enable_debug_layer = has_flag(argc, argv, "--debug-layer");
  const bool disable_vsync = has_flag(argc, argv, "--no-vsync");
  const bool force_vsync = has_flag(argc, argv, "--vsync");
  const bool enable_diagnostics = has_flag(argc, argv, "--diagnostics");
  const bool enable_vsync = force_vsync ? true : (disable_vsync ? false : !frame_limit.has_value());

  const HINSTANCE instance = GetModuleHandleW(nullptr);
  const wchar_t* class_name = L"IGRWin32Dx11SampleWindow";

  WNDCLASSEXW window_class{};
  window_class.cbSize = sizeof(window_class);
  window_class.hInstance = instance;
  window_class.lpfnWndProc = window_proc;
  window_class.lpszClassName = class_name;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));

  if (RegisterClassExW(&window_class) == 0) {
    show_error_box(nullptr, "IGR DX11 Sample", "Failed to register the sample window class.");
    return 1;
  }

  HWND window_handle = CreateWindowExW(
      0,
      class_name,
      L"IGR UI DX11 Sample",
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
    show_error_box(nullptr, "IGR DX11 Sample", "Failed to create the sample window.");
    UnregisterClassW(class_name, instance);
    return 1;
  }

  igr::backends::Dx11Theme theme;
  theme.window_background = {0.09f, 0.11f, 0.15f, 0.97f};
  theme.title_bar = {0.12f, 0.47f, 0.80f, 1.0f};
  theme.panel_background = {0.05f, 0.06f, 0.09f, 0.94f};
  theme.text_primary = {0.95f, 0.97f, 1.0f, 1.0f};
  theme.text_secondary = {0.73f, 0.79f, 0.89f, 1.0f};
  theme.button_background = {0.19f, 0.33f, 0.69f, 0.98f};
  theme.button_highlight = {0.25f, 0.43f, 0.85f, 1.0f};
  theme.progress_fill = {0.20f, 0.78f, 0.61f, 1.0f};
  theme.checkbox_fill = {0.24f, 0.67f, 0.98f, 1.0f};
  theme.body_text_size = 15.0f;
  theme.title_text_size = 16.0f;

  igr::backends::Dx11Backend backend({
      .diagnostics = {.enabled = enable_diagnostics},
      .window_handle = window_handle,
      .initial_viewport = {1280, 720},
      .enable_debug_layer = enable_debug_layer,
      .enable_vsync = enable_vsync,
      .clear_color = {0.03f, 0.04f, 0.06f, 1.0f},
      .theme = theme,
  });

  AppState app_state{
      .backend = &backend,
      .viewport = {1280, 720},
      .pending_resize = std::nullopt,
      .minimized = false,
      .fatal_error = {},
  };
  SetWindowLongPtrW(window_handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app_state));

  const igr::Status initialize_status = backend.initialize();
  if (!initialize_status) {
    show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend initialization failed: " + initialize_status.message());
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  const igr::Status font_status = backend.register_font("body-md", {
      .family = "Segoe UI",
      .size = 15.0f,
      .weight = igr::FontWeight::medium,
      .style = igr::FontStyle::normal,
      .locale = "en-us",
  });
  if (!font_status) {
    show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend font registration failed: " + font_status.message());
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  const auto demo_texture = create_demo_texture(backend.device_handle());
  if (!demo_texture) {
    show_error_box(window_handle, "IGR DX11 Sample", "Failed to create the sample image texture.");
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  const igr::Status texture_status = backend.register_texture("demo-texture", demo_texture.Get());
  if (!texture_status) {
    show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend texture registration failed: " + texture_status.message());
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  const igr::Status image_status = backend.register_image("demo-card", {
      .texture_key = "demo-texture",
      .size = {180.0f, 84.0f},
      .uv = {{0.0f, 0.0f}, {1.0f, 1.0f}},
      .tint = {1.0f, 1.0f, 1.0f, 1.0f},
  });
  if (!image_status) {
    show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend image registration failed: " + image_status.message());
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  const igr::Status shader_status = backend.register_shader("demo-shader", make_demo_shader());
  if (!shader_status) {
    show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend shader registration failed: " + shader_status.message());
    backend.shutdown();
    DestroyWindow(window_handle);
    UnregisterClassW(class_name, instance);
    return 1;
  }

  ShowWindow(window_handle, SW_SHOWDEFAULT);
  UpdateWindow(window_handle);

  log_debug("DX11 sample running. owns_device_objects=" + std::string(backend.owns_device_objects() ? "true" : "false") +
            " vsync=" + (enable_vsync ? std::string("true") : std::string("false")) +
            " debug_layer=" + (enable_debug_layer ? std::string("true") : std::string("false")));

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
      show_error_box(window_handle, "IGR DX11 Sample", app_state.fatal_error);
      running = false;
      break;
    }

    if (app_state.pending_resize.has_value()) {
      const igr::Status resize_status = backend.resize(*app_state.pending_resize);
      if (!resize_status) {
        show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend resize failed: " + resize_status.message());
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

    std::optional<igr::BackendTelemetrySnapshot> telemetry;
    if (enable_diagnostics) {
      telemetry = backend.telemetry();
    }
    const igr::FrameDocument document = build_demo_document(++frame_index, elapsed_seconds, delta_seconds, app_state.viewport, "demo-texture",
                                                            "demo-card", telemetry ? &*telemetry : nullptr);
    igr::Status status = backend.render(document);
    if (!status) {
      show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend render failed: " + status.message());
      running = false;
      break;
    }

    status = backend.present();
    if (!status) {
      show_error_box(window_handle, "IGR DX11 Sample", "Dx11Backend present failed: " + status.message());
      running = false;
      break;
    }

    if (frame_limit.has_value() && frame_index >= *frame_limit) {
      DestroyWindow(window_handle);
    }
  }

  backend.shutdown();
  if (!app_state.fatal_error.empty()) {
    if (IsWindow(window_handle)) {
      DestroyWindow(window_handle);
    }
    UnregisterClassW(class_name, instance);
    return 1;
  }
  if (IsWindow(window_handle)) {
    DestroyWindow(window_handle);
  }
  UnregisterClassW(class_name, instance);
  return 0;
}

extern "C" int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  return run_application(__argc, __argv);
}


