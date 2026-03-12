#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "igr/context.hpp"

namespace igr::react {

using PropertyValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

struct Property {
  std::string name;
  PropertyValue value;
};

struct ElementNode {
  std::string type;
  std::string key;
  std::vector<Property> props;
  std::vector<ElementNode> children;
};

Status materialize(const ElementNode& element, FrameBuilder& builder);

}  // namespace igr::react

