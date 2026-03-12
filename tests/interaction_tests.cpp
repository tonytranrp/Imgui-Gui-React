#include <iostream>

#include "igr/context.hpp"
#include "igr/interaction.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

igr::FrameDocument build_document() {
  igr::UiContext context;
  context.begin_frame({
      .frame_index = 1,
      .viewport = {1280, 720},
      .delta_seconds = 1.0 / 60.0,
  });

  auto& builder = context.builder();
  builder.begin_window("overlay", "Overlay", {{32.0f, 32.0f}, {320.0f, 220.0f}});
  builder.begin_stack("layout", igr::Axis::vertical);
  builder.text("headline", "Overlay host capture");
  builder.button("cta", "Commit");
  builder.begin_clip_rect("clip", {200.0f, 96.0f});
  builder.checkbox("enabled", "Enabled", true);
  builder.image("preview", "demo-texture", {96.0f, 48.0f}, "Preview");
  builder.fill_rect("accent-fill", {{8.0f, 8.0f}, {20.0f, 8.0f}}, {0.25f, 0.70f, 0.98f, 0.85f});
  builder.draw_line("accent-line", {4.0f, 44.0f}, {84.0f, 44.0f}, {0.12f, 0.18f, 0.22f, 1.0f}, 2.0f);
  builder.end_container();
  builder.end_container();
  builder.end_container();
  return context.end_frame();
}

}  // namespace

int main() {
  const igr::FrameDocument document = build_document();
  const igr::InteractionMap map = igr::build_interaction_map(document);
  if (map.regions.size() != 4) {
    return fail("interaction map did not materialize the expected window and interactive regions");
  }

  igr::CaptureDecision button_capture = igr::evaluate_capture(
      document,
      {.input_mode = igr::InputMode::external_forwarded},
      {
          .position = {64.0f, 120.0f},
          .primary_down = true,
          .keyboard_requested = true,
      });
  if (!button_capture.within_window || !button_capture.within_interactive_region || !button_capture.wants_mouse || !button_capture.wants_keyboard ||
      button_capture.active_widget_id == 0) {
    return fail("button-region capture was not detected correctly");
  }

  igr::CaptureDecision window_capture = igr::evaluate_capture(
      document,
      {.input_mode = igr::InputMode::external_forwarded},
      {
          .position = {64.0f, 56.0f},
          .primary_down = false,
          .keyboard_requested = false,
      });
  if (!window_capture.within_window || window_capture.within_interactive_region || !window_capture.wants_mouse) {
    return fail("window-region capture was not detected correctly");
  }

  igr::CaptureDecision disabled_capture = igr::evaluate_capture(
      document,
      {.input_mode = igr::InputMode::none},
      {
          .position = {64.0f, 120.0f},
          .primary_down = true,
          .keyboard_requested = true,
      });
  if (disabled_capture.wants_mouse || disabled_capture.wants_keyboard) {
    return fail("capture should remain disabled when input_mode is none");
  }

  std::cout << "igr_interaction_tests passed" << '\n';
  return 0;
}
