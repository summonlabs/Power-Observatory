// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// A deliberately small test harness. It has no external dependency, reports
// precise failure locations, and never imposes a timeout: a test that hangs is
// a defect to be diagnosed, not a test to be killed.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "power_observatory/reason.hpp"
#include "power_observatory/result.hpp"

namespace potest {

struct TestCase {
  const char* suite;
  const char* name;
  void (*function)();
};

struct Registration {
  Registration(const char* suite, const char* name, void (*function)());
};

[[nodiscard]] std::vector<TestCase>& registry();
[[nodiscard]] int run_all(int argc, char** argv);

// Raised by PO_REQUIRE-style macros: aborts the current case but not the run.
struct AssertionFailure {
  std::string message;
};

void report_failure(std::string message_with_location);
void report_failure_at(const char* file, int line, std::string message);

template <class T>
[[nodiscard]] std::string to_debug_string(const T& value) {
  if constexpr (std::is_same_v<T, std::string>) {
    return value;
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    return std::string(value);
  } else if constexpr (std::is_same_v<T, const char*> || std::is_same_v<T, char*>) {
    return value == nullptr ? std::string("<null>") : std::string(value);
  } else if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(value));
  } else if constexpr (std::is_integral_v<T>) {
    return std::to_string(value);
  } else {
    return "<value>";
  }
}

[[nodiscard]] inline std::string describe_error(const po::Error& error) {
  return std::string(po::to_string(error.code())) + " (" + error.detail() + ")";
}

}  // namespace potest

#define PO_TEST(suite_name, case_name)                                                        \
  static void po_test_case_##suite_name##_##case_name();                                      \
  static const ::potest::Registration po_test_registration_##suite_name##_##case_name(        \
      #suite_name, #case_name, &po_test_case_##suite_name##_##case_name);                     \
  static void po_test_case_##suite_name##_##case_name()

#define PO_FAIL(message) ::potest::report_failure_at(__FILE__, __LINE__, (message))

#define PO_CHECK(expression)                                                                  \
  do {                                                                                        \
    if (!(expression)) {                                                                      \
      PO_FAIL(std::string("check failed: ") + #expression);                                   \
    }                                                                                         \
  } while (false)

#define PO_REQUIRE(expression)                                                                \
  do {                                                                                        \
    if (!(expression)) {                                                                      \
      throw ::potest::AssertionFailure{std::string("requirement failed: ") + #expression};    \
    }                                                                                         \
  } while (false)

#define PO_CHECK_EQ(actual, expected)                                                         \
  do {                                                                                        \
    const auto& po_actual_value = (actual);                                                   \
    const auto& po_expected_value = (expected);                                               \
    if (!(po_actual_value == po_expected_value)) {                                            \
      PO_FAIL(std::string("expected ") + #actual + " == " + #expected + ", got " +            \
              ::potest::to_debug_string(po_actual_value) + " vs " +                           \
              ::potest::to_debug_string(po_expected_value));                                  \
    }                                                                                         \
  } while (false)

#define PO_CHECK_NE(actual, unexpected)                                                       \
  do {                                                                                        \
    const auto& po_actual_value = (actual);                                                   \
    const auto& po_unexpected_value = (unexpected);                                           \
    if (po_actual_value == po_unexpected_value) {                                             \
      PO_FAIL(std::string("expected ") + #actual + " != " + #unexpected);                     \
    }                                                                                         \
  } while (false)

#define PO_REQUIRE_OK(expression)                                                             \
  do {                                                                                        \
    const auto& po_result_value = (expression);                                               \
    if (!po_result_value.has_value()) {                                                       \
      throw ::potest::AssertionFailure{std::string("expected success from ") + #expression +  \
                                       ", got " + ::potest::describe_error(po_result_value.error())}; \
    }                                                                                         \
  } while (false)

#define PO_REQUIRE_ERR(expression, expected_code)                                             \
  do {                                                                                        \
    const auto& po_result_value = (expression);                                               \
    if (po_result_value.has_value()) {                                                        \
      throw ::potest::AssertionFailure{std::string("expected failure from ") + #expression};  \
    }                                                                                         \
    if (po_result_value.error().code() != (expected_code)) {                                  \
      throw ::potest::AssertionFailure{                                                       \
          std::string("expected ") + #expression + " to fail with " +                         \
          std::string(::po::to_string(expected_code)) + ", got " +                            \
          ::potest::describe_error(po_result_value.error())};                                 \
    }                                                                                         \
  } while (false)

#define PO_CHECK_OK(expression)                                                               \
  do {                                                                                        \
    const auto& po_result_value = (expression);                                               \
    if (!po_result_value.has_value()) {                                                       \
      PO_FAIL(std::string("expected success from ") + #expression + ", got " +                \
              ::potest::describe_error(po_result_value.error()));                             \
    }                                                                                         \
  } while (false)

#define PO_TEST_MAIN()                                                                        \
  int main(int argc, char** argv) { return ::potest::run_all(argc, argv); }
