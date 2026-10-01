// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/version.hpp"

#include <string>

namespace po {
namespace {

constexpr std::string_view kCompilerName =
#if defined(_MSC_VER)
    "msvc"
#elif defined(__clang__)
    "clang"
#elif defined(__GNUC__)
    "gcc"
#else
    "unknown"
#endif
    ;

constexpr std::string_view kCxxStandard =
#if defined(_MSVC_LANG)
    _MSVC_LANG >= 202002L ? "c++20" : "pre-c++20"
#elif defined(__cplusplus)
    __cplusplus >= 202002L ? "c++20" : "pre-c++20"
#else
    "unknown"
#endif
    ;

constexpr std::string_view kBuildMode =
#if defined(NDEBUG)
    "release"
#else
    "debug"
#endif
    ;

constexpr std::string_view kSanitizer =
#if defined(__SANITIZE_ADDRESS__)
    "asan"
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    "asan"
#else
    "none"
#endif
#else
    "none"
#endif
    ;

// Assembled at first use so that build_info() returns a stable, allocation-free
// view that is identical in every process built the same way.
const std::string& assembled() {
  static const std::string value = [] {
    std::string text;
    text.reserve(96);
    text.append(kProjectName);
    text.append(" ");
    text.append(kVersionString);
    text.append(" compiler=");
    text.append(kCompilerName);
    text.append(" standard=");
    text.append(kCxxStandard);
    text.append(" mode=");
    text.append(kBuildMode);
    text.append(" sanitizer=");
    text.append(kSanitizer);
    return text;
  }();
  return value;
}

}  // namespace

std::string_view build_info() noexcept { return assembled(); }

}  // namespace po
