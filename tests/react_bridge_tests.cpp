#include <iostream>
#include <string_view>

#include "igr/context.hpp"
#include "igr/react/document.hpp"

namespace {

int fail(const char* message) {
  std::cerr << message << '\n';
  return 1;
}

const igr::WidgetAttribute* find_attribute(const igr::WidgetNode& node, std::string_view name) {
  for (const auto& attribute : node.attributes) {
    if (attribute.name == name) {
      return &attribute;
    }
  }
  return nullptr;
}

}  // namespace

int main() {
  igr::UiContext context;
  auto status = context.begin_frame({
      .frame_index = 9,
      .viewport = {1280, 720},
      .delta_seconds = 1.0 / 60.0,
  });

  if (!status) {
    return fail("begin_frame failed");
  }

  igr::react::ElementNode root{
      .type = "window",
      .key = "react-root",
      .props = {
          {"title", std::string("Bridge Root")},
          {"x", 50.0},
          {"y", 50.0},
          {"width", 360.0},
          {"height", 240.0},
      },
      .children = {
          {
              .type = "stack",
              .key = "column",
              .props = {{"axis", std::string("vertical")}},
              .children = {
                  {.type = "text", .key = "headline", .props = {{"value", std::string("Bridge text")}, {"font", std::string("body-md")}}},
                  {.type = "button", .key = "cta", .props = {{"label", std::string("Bridge button")}, {"enabled", true}}},
                  {
                      .type = "clip_rect",
                      .key = "preview-clip",
                      .props = {{"width", 110.0}, {"height", 64.0}},
                      .children = {
                          {.type = "checkbox", .key = "enabled", .props = {{"label", std::string("Bridge toggle")}, {"checked", true}}},
                          {.type = "shader_rect",
                           .key = "shader-preview",
                           .props = {
                               {"shader", std::string("bridge-pulse")},
                               {"x", 4.0},
                               {"y", 6.0},
                               {"width", 82.0},
                               {"height", 32.0},
                               {"resource", std::string("bridge-card")},
                               {"texture", std::string("bridge-image")},
                               {"tint", std::string("#8FD0FFFF")},
                               {"param0", std::string("0.2, 0.6, 1.0, 0.0")},
                               {"param1", std::string("1.0, 0.0, 0.0, 0.0")},
                           }},
                          {.type = "image", .key = "preview", .props = {{"texture", std::string("bridge-image")}, {"resource", std::string("bridge-card")}, {"width", 96.0}, {"height", 48.0}, {"label", std::string("Bridge preview")}}},
                          {.type = "fill_rect", .key = "fill-rect", .props = {{"x", 8.0}, {"y", 8.0}, {"width", 20.0}, {"height", 10.0}, {"color", std::string("#2FA9F2D8")}}},
                          {.type = "line", .key = "bridge-line", .props = {{"x1", 6.0}, {"y1", 44.0}, {"x2", 86.0}, {"y2", 44.0}, {"thickness", 2.0}, {"color", std::string("#203344FF")}}},
                      },
                  },
                  {.type = "progress_bar", .key = "sync", .props = {{"label", std::string("Bridge sync")}, {"value", 1.25}}},
                  {.type = "separator", .key = "line"},
              },
          },
      },
  };

  status = igr::react::materialize(root, context.builder());
  if (!status) {
    std::cerr << status.message() << '\n';
    return fail("materialize failed");
  }

  const auto document = context.end_frame();
  if (document.roots.size() != 1 || document.widget_count() != 12) {
    return fail("unexpected document shape");
  }

  const auto& root_window = document.roots.front();
  if (root_window.label != "Bridge Root") {
    return fail("window title did not materialize");
  }

  if (root_window.children.front().children.front().label != "Bridge text" ||
      root_window.children.front().children.front().attributes.front().value != "body-md") {
    return fail("text node did not materialize");
  }

  const auto& bridge_clip_rect = root_window.children.front().children[2];
  if (bridge_clip_rect.kind != igr::WidgetKind::clip_rect) {
    return fail("clip rect node did not materialize");
  }
  if (const auto* height_attr = find_attribute(bridge_clip_rect, "height"); height_attr == nullptr || height_attr->value != "64") {
    return fail("clip rect size did not materialize");
  }

  const auto& bridge_checkbox = bridge_clip_rect.children[0];
  if (bridge_checkbox.kind != igr::WidgetKind::checkbox || bridge_checkbox.attributes.front().value != "true") {
    return fail("checkbox node did not materialize");
  }

  const auto& bridge_shader = bridge_clip_rect.children[1];
  if (bridge_shader.kind != igr::WidgetKind::custom_draw) {
    return fail("shader_rect node did not materialize");
  }
  if (const auto* primitive = find_attribute(bridge_shader, "primitive"); primitive == nullptr || primitive->value != "shader_rect") {
    return fail("shader_rect primitive was not preserved");
  }
  if (const auto* shader = find_attribute(bridge_shader, "shader"); shader == nullptr || shader->value != "bridge-pulse") {
    return fail("shader_rect shader key was not preserved");
  }
  if (const auto* resource = find_attribute(bridge_shader, "resource"); resource == nullptr || resource->value != "bridge-card") {
    return fail("shader_rect resource key was not preserved");
  }

  const auto& bridge_image = bridge_clip_rect.children[2];
  if (bridge_image.kind != igr::WidgetKind::image) {
    return fail("image node did not materialize");
  }
  if (const auto* texture = find_attribute(bridge_image, "texture"); texture == nullptr || texture->value != "bridge-image") {
    return fail("image texture did not materialize");
  }
  if (const auto* resource = find_attribute(bridge_image, "resource"); resource == nullptr || resource->value != "bridge-card") {
    return fail("image resource did not materialize");
  }

  if (bridge_clip_rect.children[3].kind != igr::WidgetKind::custom_draw || bridge_clip_rect.children[3].attributes.front().value != "fill_rect") {
    return fail("fill-rect node did not materialize");
  }

  if (bridge_clip_rect.children[4].kind != igr::WidgetKind::custom_draw || bridge_clip_rect.children[4].attributes.front().value != "line") {
    return fail("line node did not materialize");
  }

  const auto& bridge_progress = root_window.children.front().children[3];
  if (bridge_progress.kind != igr::WidgetKind::progress_bar || bridge_progress.attributes.front().value != "1") {
    return fail("progress node did not materialize or clamp correctly");
  }

  std::cout << "igr_react_bridge_tests passed" << '\n';
  return 0;
}
