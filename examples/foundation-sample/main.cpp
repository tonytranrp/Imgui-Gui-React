#include <iostream>
#include <string>

#include "igr/context.hpp"
#include "igr/react/document.hpp"

namespace {

void print_node(const igr::WidgetNode& node, int depth) {
  std::cout << std::string(static_cast<std::size_t>(depth) * 2, ' ')
            << igr::to_string(node.kind) << " key=" << node.key << " label=" << node.label << '\n';

  for (const auto& child : node.children) {
    print_node(child, depth + 1);
  }
}

}  // namespace

int main() {
  igr::UiContext context;

  const igr::Status begin_status = context.begin_frame({
      .frame_index = 1,
      .viewport = {1280, 720},
      .delta_seconds = 1.0 / 60.0,
  });

  if (!begin_status) {
    std::cerr << "Failed to begin frame: " << begin_status.message() << '\n';
    return 1;
  }

  auto& builder = context.builder();
  builder.begin_window("native-main", "Native Foundation Window", {{24.0f, 24.0f}, {420.0f, 220.0f}});
  builder.begin_stack("native-stack", igr::Axis::vertical);
  builder.text("intro-text", "Immediate-mode foundation ready.");
  builder.button("launch-button", "Render the next milestone");
  builder.end_container();
  builder.end_container();

  igr::react::ElementNode bridge_window{
      .type = "window",
      .key = "bridge-main",
      .props = {
          {"title", std::string("Declarative Bridge Window")},
          {"x", 480.0},
          {"y", 24.0},
          {"width", 420.0},
          {"height", 220.0},
      },
      .children = {
          {
              .type = "stack",
              .key = "bridge-stack",
              .props = {{"axis", std::string("vertical")}},
              .children = {
                  {.type = "text", .key = "bridge-text", .props = {{"value", std::string("React-style tree materialized into the frame.")}}},
                  {.type = "button", .key = "bridge-button", .props = {{"label", std::string("Inspect output")}, {"enabled", true}}},
              },
          },
      },
  };

  const igr::Status bridge_status = igr::react::materialize(bridge_window, builder);
  if (!bridge_status) {
    std::cerr << "Bridge materialization failed: " << bridge_status.message() << '\n';
    return 1;
  }

  const igr::FrameDocument document = context.end_frame();

  std::cout << "Frame " << document.info.frame_index << " produced " << document.widget_count() << " widgets" << '\n';
  for (const auto& root : document.roots) {
    print_node(root, 0);
  }

  for (const auto& diagnostic : document.diagnostics) {
    std::cout << "diagnostic: " << diagnostic << '\n';
  }

  return 0;
}

