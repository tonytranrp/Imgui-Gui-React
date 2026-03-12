#include <iostream>

#include "igr/context.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

}  // namespace

int main() {
  igr::UiContext context;
  auto status = context.begin_frame({
      .frame_index = 7,
      .viewport = {1920, 1080},
      .delta_seconds = 1.0 / 120.0,
  });

  if (!status) {
    return fail("begin_frame failed");
  }

  auto& builder = context.builder();
  builder.begin_window("main", "Main", {{10.0f, 20.0f}, {300.0f, 200.0f}});
  builder.begin_stack("content", igr::Axis::vertical);
  builder.text("title", "Hello");
  builder.button("action", "Click");
  builder.begin_clip_rect("preview-clip", {96.0f, 56.0f});
  builder.checkbox("enabled", "Feature Enabled", true);
  builder.image("thumbnail", "demo-texture", {96.0f, 48.0f}, "Preview");
  igr::ShaderUniformData shader_rect_uniforms{};
  shader_rect_uniforms.tint = {0.55f, 0.80f, 1.0f, 0.92f};
  shader_rect_uniforms.params[0] = {7.0f, 1.0f / 120.0f, 1920.0f, 1080.0f};
  builder.shader_rect("shader-surface", "pulse-shader", {{8.0f, 12.0f}, {72.0f, 18.0f}}, {}, {}, shader_rect_uniforms);
  igr::ShaderUniformData shader_image_uniforms{};
  shader_image_uniforms.tint = {1.0f, 0.95f, 0.88f, 1.0f};
  shader_image_uniforms.params[0] = {0.25f, 0.50f, 0.75f, 1.0f};
  builder.shader_image("shader-thumbnail", "pulse-shader", "demo-texture", {96.0f, 48.0f}, "Shader Preview", "demo-card", shader_image_uniforms);
  builder.fill_rect("preview-fill", {{8.0f, 8.0f}, {24.0f, 10.0f}}, {0.25f, 0.70f, 0.98f, 0.85f});
  builder.draw_line("preview-rule", {4.0f, 42.0f}, {84.0f, 42.0f}, {0.12f, 0.18f, 0.22f, 1.0f}, 2.0f);
  builder.end_container();
  builder.progress_bar("load", "Load", 1.25f);
  builder.end_container();
  builder.end_container();

  const auto document_a = context.end_frame();
  if (document_a.roots.size() != 1 || document_a.widget_count() != 12) {
    return fail("unexpected frame shape in first frame");
  }

  const auto& content_a = document_a.roots.front().children.front();
  if (content_a.children.size() != 4) {
    return fail("unexpected number of widgets in the main content stack");
  }

  if (content_a.children[2].kind != igr::WidgetKind::clip_rect || content_a.children[2].attributes[1].value != "56") {
    return fail("clip rect widget attributes did not materialize correctly");
  }

  if (content_a.children[2].children[0].kind != igr::WidgetKind::checkbox || content_a.children[2].children[0].attributes.front().value != "true") {
    return fail("checkbox widget state did not materialize correctly");
  }

  if (content_a.children[2].children[1].kind != igr::WidgetKind::image || content_a.children[2].children[1].attributes.front().value != "demo-texture") {
    return fail("image widget attributes did not materialize correctly");
  }

  if (content_a.children[2].children[2].kind != igr::WidgetKind::custom_draw ||
      content_a.children[2].children[2].attributes.front().value != "shader_rect") {
    return fail("custom shader-rect widget did not materialize correctly");
  }

  if (content_a.children[2].children[3].kind != igr::WidgetKind::custom_draw ||
      content_a.children[2].children[3].attributes.front().value != "shader_image") {
    return fail("custom shader-image widget did not materialize correctly");
  }

  if (content_a.children[2].children[4].kind != igr::WidgetKind::custom_draw ||
      content_a.children[2].children[4].attributes.front().value != "fill_rect") {
    return fail("custom fill-rect widget did not materialize correctly");
  }

  if (content_a.children[2].children[5].kind != igr::WidgetKind::custom_draw ||
      content_a.children[2].children[5].attributes.front().value != "line") {
    return fail("custom line widget did not materialize correctly");
  }

  if (content_a.children[3].kind != igr::WidgetKind::progress_bar || content_a.children[3].attributes.front().value != "1") {
    return fail("progress bar widget did not clamp into the expected range");
  }

  const auto root_id_a = document_a.roots.front().id;
  const auto button_id_a = content_a.children[1].id;

  status = context.begin_frame({
      .frame_index = 8,
      .viewport = {1920, 1080},
      .delta_seconds = 1.0 / 120.0,
  });
  if (!status) {
    return fail("second begin_frame failed");
  }

  builder.begin_window("main", "Main", {{10.0f, 20.0f}, {300.0f, 200.0f}});
  builder.begin_stack("content", igr::Axis::vertical);
  builder.text("title", "Hello");
  builder.button("action", "Click");
  builder.begin_clip_rect("preview-clip", {96.0f, 56.0f});
  builder.checkbox("enabled", "Feature Enabled", true);
  builder.image("thumbnail", "demo-texture", {96.0f, 48.0f}, "Preview");
  builder.shader_rect("shader-surface", "pulse-shader", {{8.0f, 12.0f}, {72.0f, 18.0f}});
  builder.shader_image("shader-thumbnail", "pulse-shader", "demo-texture", {96.0f, 48.0f}, "Shader Preview", "demo-card");
  builder.fill_rect("preview-fill", {{8.0f, 8.0f}, {24.0f, 10.0f}}, {0.25f, 0.70f, 0.98f, 0.85f});
  builder.draw_line("preview-rule", {4.0f, 42.0f}, {84.0f, 42.0f}, {0.12f, 0.18f, 0.22f, 1.0f}, 2.0f);
  builder.end_container();
  builder.progress_bar("load", "Load", 1.25f);
  builder.end_container();
  builder.end_container();

  const auto document_b = context.end_frame();
  const auto root_id_b = document_b.roots.front().id;
  const auto button_id_b = document_b.roots.front().children.front().children[1].id;

  if (root_id_a != root_id_b || button_id_a != button_id_b) {
    return fail("widget identifiers are not deterministic");
  }

  if (!document_b.diagnostics.empty()) {
    return fail("unexpected diagnostics in closed frame");
  }

  std::cout << "igr_core_tests passed" << '\n';
  return 0;
}
