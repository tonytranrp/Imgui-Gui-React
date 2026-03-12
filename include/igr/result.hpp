#pragma once

#include <string>

namespace igr {

enum class StatusCode {
  ok,
  invalid_argument,
  not_ready,
  unsupported,
  backend_error,
};

class Status {
 public:
  static Status success();
  static Status invalid_argument(std::string message);
  static Status not_ready(std::string message);
  static Status unsupported(std::string message);
  static Status backend_error(std::string message);

  [[nodiscard]] bool ok() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;
  [[nodiscard]] StatusCode code() const noexcept;
  [[nodiscard]] const std::string& message() const noexcept;

 private:
  Status(StatusCode code, std::string message);

  StatusCode code_{StatusCode::ok};
  std::string message_{};
};

}  // namespace igr
