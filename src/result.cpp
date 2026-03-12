#include "igr/result.hpp"

#include <utility>

namespace igr {

Status Status::success() {
  return Status(StatusCode::ok, {});
}

Status Status::invalid_argument(std::string message) {
  return Status(StatusCode::invalid_argument, std::move(message));
}

Status Status::not_ready(std::string message) {
  return Status(StatusCode::not_ready, std::move(message));
}

Status Status::unsupported(std::string message) {
  return Status(StatusCode::unsupported, std::move(message));
}

Status Status::backend_error(std::string message) {
  return Status(StatusCode::backend_error, std::move(message));
}

bool Status::ok() const noexcept {
  return code_ == StatusCode::ok;
}

Status::operator bool() const noexcept {
  return ok();
}

StatusCode Status::code() const noexcept {
  return code_;
}

const std::string& Status::message() const noexcept {
  return message_;
}

Status::Status(StatusCode code, std::string message)
    : code_(code), message_(std::move(message)) {}

}  // namespace igr

