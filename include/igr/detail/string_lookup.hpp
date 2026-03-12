#pragma once

#include <algorithm>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace igr::detail {

struct TransparentStringHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  std::size_t operator()(const std::string& value) const noexcept {
    return operator()(std::string_view(value));
  }

  std::size_t operator()(const char* value) const noexcept {
    return value == nullptr ? 0u : operator()(std::string_view(value));
  }
};

struct TransparentStringEqual {
  using is_transparent = void;

  bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
    return lhs == rhs;
  }
};

template <typename Value>
using TransparentStringMap = std::unordered_map<std::string, Value, TransparentStringHash, TransparentStringEqual>;

template <typename T, typename Allocator>
void release_storage(std::vector<T, Allocator>& values) {
  std::vector<T, Allocator>{}.swap(values);
}

template <typename T, typename Allocator>
void trim_storage(std::vector<T, Allocator>& values, std::size_t desired_capacity) {
  desired_capacity = std::max(desired_capacity, values.size());
  if (values.capacity() <= desired_capacity) {
    return;
  }

  std::vector<T, Allocator> trimmed;
  trimmed.reserve(desired_capacity);
  trimmed.insert(trimmed.end(),
                 std::make_move_iterator(values.begin()),
                 std::make_move_iterator(values.end()));
  values.swap(trimmed);
}

template <typename Key, typename Value, typename Hash, typename KeyEqual, typename Allocator>
void release_storage(std::unordered_map<Key, Value, Hash, KeyEqual, Allocator>& values) {
  std::unordered_map<Key, Value, Hash, KeyEqual, Allocator>{}.swap(values);
}

template <typename Key, typename Value, typename Hash, typename KeyEqual, typename Allocator>
void trim_storage(std::unordered_map<Key, Value, Hash, KeyEqual, Allocator>& values, std::size_t desired_size) {
  if (values.empty()) {
    release_storage(values);
    return;
  }

  desired_size = std::max(desired_size, values.size());
  std::unordered_map<Key, Value, Hash, KeyEqual, Allocator> trimmed;
  trimmed.max_load_factor(values.max_load_factor());
  trimmed.reserve(desired_size);
  for (auto& entry : values) {
    trimmed.emplace(std::move(entry.first), std::move(entry.second));
  }
  values.swap(trimmed);
}

}  // namespace igr::detail
