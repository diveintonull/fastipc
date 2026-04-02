#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace fastipc {

enum class StatusCode : std::uint8_t {
  Ok = 0,
  InvalidArgument,
  AlreadyExists,
  NotFound,
  PermissionDenied,
  LayoutMismatch,
  RoleConflict,
  MessageTooLarge,
  BufferTooSmall,
  WouldBlock,
  Timeout,
  Dropped,
  PeerUnavailable,
  PeerDead,
  StaleGeneration,
  CorruptData,
  Closed,
  IoError,
  Unsupported,
};

class Status {
 public:
  Status() = default;
  Status(StatusCode code, std::string detail = {}, int native_error = 0)
      : code_(code), native_error_(native_error), detail_(std::move(detail)) {}

  [[nodiscard]] static Status Ok() { return {}; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] int native_error() const noexcept { return native_error_; }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
  [[nodiscard]] std::string ToString() const;

 private:
  StatusCode code_{StatusCode::Ok};
  int native_error_{0};
  std::string detail_;
};

template <typename T>
class Result {
 public:
  Result(T value) : value_(std::move(value)), status_(Status::Ok()) {}
  Result(Status status) : status_(std::move(status)) {
    if (status_.ok()) {
      throw std::invalid_argument("an error Result requires a non-OK status");
    }
  }

  [[nodiscard]] bool ok() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] const Status& status() const noexcept { return status_; }

  [[nodiscard]] T& value() & {
    EnsureValue();
    return *value_;
  }

  [[nodiscard]] const T& value() const& {
    EnsureValue();
    return *value_;
  }

  [[nodiscard]] T&& take_value() && {
    EnsureValue();
    return std::move(*value_);
  }

 private:
  void EnsureValue() const {
    if (!value_) {
      throw std::logic_error("attempted to read an error Result");
    }
  }

  std::optional<T> value_;
  Status status_;
};

}  // namespace fastipc
