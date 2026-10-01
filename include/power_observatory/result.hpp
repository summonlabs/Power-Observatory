// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <string>
#include <utility>
#include <variant>

#include "power_observatory/reason.hpp"

namespace po {

// A refusal. Carries a machine-readable reason code and a human-readable
// detail string. Detail strings are diagnostic only and are never parsed by
// consumers; every decision must key off the code.
class Error {
 public:
  Error() = default;

  Error(ReasonCode code, std::string detail) : code_(code), detail_(std::move(detail)) {}

  explicit Error(ReasonCode code) : code_(code) {}

  [[nodiscard]] ReasonCode code() const noexcept { return code_; }
  [[nodiscard]] Severity severity() const noexcept { return po::severity(code_); }
  [[nodiscard]] const std::string& detail() const noexcept { return detail_; }

  [[nodiscard]] bool operator==(const Error& other) const noexcept {
    return code_ == other.code_ && detail_ == other.detail_;
  }

 private:
  ReasonCode code_{ReasonCode::None};
  std::string detail_;
};

[[nodiscard]] inline Error make_error(ReasonCode code, std::string detail) {
  return Error(code, std::move(detail));
}

// Result<T> is either a value or an Error. Constructing from T is implicit so
// helpers can simply return a value; constructing from Error is implicit so
// failures read cleanly at the call site.
//
// Result is [[nodiscard]]: silently dropping a failure is a defect, and the
// compiler is asked to enforce that. There is no "empty" state for a
// non-void Result, so a success always carries an actual value.
template <class T>
class [[nodiscard]] Result {
 public:
  using value_type = T;
  using error_type = Error;

  Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == 0; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] T& value() & { return std::get<0>(storage_); }
  [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
  [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

  [[nodiscard]] Error& error() & { return std::get<1>(storage_); }
  [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }

  [[nodiscard]] ReasonCode code() const noexcept { return std::get<1>(storage_).code(); }
  [[nodiscard]] const std::string& detail() const noexcept { return std::get<1>(storage_).detail(); }

  [[nodiscard]] T value_or(T fallback) const {
    return has_value() ? std::get<0>(storage_) : std::move(fallback);
  }

  // Replace the detail of a failure while preserving the code. No-op on success.
  Result& with_detail(std::string detail) & {
    if (!has_value()) {
      storage_ = Error(code(), std::move(detail));
    }
    return *this;
  }

  // Transform the contained value. The callable runs only on success.
  template <class F>
  auto map(F&& f) const -> Result<decltype(f(std::declval<const T&>()))> {
    using U = decltype(f(std::declval<const T&>()));
    if (has_value()) {
      return Result<U>(f(std::get<0>(storage_)));
    }
    return Result<U>(std::get<1>(storage_));
  }

  // Chain a fallible operation. The callable runs only on success.
  template <class F>
  auto and_then(F&& f) const -> decltype(f(std::declval<const T&>())) {
    using R = decltype(f(std::declval<const T&>()));
    if (has_value()) {
      return f(std::get<0>(storage_));
    }
    return R(std::get<1>(storage_));
  }

 private:
  std::variant<T, Error> storage_;
};

// The void-valued result. Success carries nothing; failure carries an Error.
template <>
class [[nodiscard]] Result<void> {
 public:
  using value_type = void;
  using error_type = Error;

  Result() = default;
  Result(Error error) : error_(std::move(error)), ok_(false) {}

  [[nodiscard]] bool has_value() const noexcept { return ok_; }
  [[nodiscard]] explicit operator bool() const noexcept { return ok_; }

  [[nodiscard]] Error& error() noexcept { return error_; }
  [[nodiscard]] const Error& error() const noexcept { return error_; }
  [[nodiscard]] ReasonCode code() const noexcept { return error_.code(); }
  [[nodiscard]] const std::string& detail() const noexcept { return error_.detail(); }

  Result& with_detail(std::string detail) {
    if (!ok_) {
      error_ = Error(error_.code(), std::move(detail));
    }
    return *this;
  }

 private:
  Error error_{};
  bool ok_{true};
};

using Status = Result<void>;

[[nodiscard]] inline Status ok_status() noexcept { return Status{}; }

[[nodiscard]] inline Status fail(ReasonCode code, std::string detail) {
  return Status(Error(code, std::move(detail)));
}

}  // namespace po
