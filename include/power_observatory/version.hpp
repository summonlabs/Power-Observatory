// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <string_view>

namespace po {

inline constexpr std::string_view kProjectName = "Power Observatory";
inline constexpr std::string_view kOrganization = "Summon Software Labs";

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";

// On-disk record log format version. Bumping this is a breaking storage change.
inline constexpr std::uint16_t kEvidenceLogFormatVersion = 1;
// Snapshot publication format version.
inline constexpr std::uint16_t kSnapshotFormatVersion = 1;
// JSON document schema version emitted by the public surfaces.
inline constexpr std::uint16_t kJsonSchemaVersion = 1;

// Sanitizer detection. A build description must never claim a sanitizer it was
// not built with, and must never omit one it was. The build system defines
// PO_SANITIZER_ASAN and PO_SANITIZER_UBSAN for exactly the sanitizers it passed,
// which is the only fully reliable source: GCC defines __SANITIZE_ADDRESS__ but
// has no macro for -fsanitize=undefined, so a combined GCC build would otherwise
// describe itself as address-only. The compiler builtins remain as the fallback
// for a translation unit compiled outside this build system.
inline constexpr bool kAddressSanitizerEnabled =
#if defined(PO_SANITIZER_ASAN) || defined(__SANITIZE_ADDRESS__)
    true
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    true
#else
    false
#endif
#else
    false
#endif
    ;

inline constexpr bool kUndefinedBehaviorSanitizerEnabled =
#if defined(PO_SANITIZER_UBSAN) || defined(__SANITIZE_UNDEFINED__)
    true
#elif defined(__has_feature)
#if __has_feature(undefined_behavior_sanitizer)
    true
#else
    false
#endif
#else
    false
#endif
    ;

inline constexpr std::string_view kSanitizerDescription =
    kAddressSanitizerEnabled && kUndefinedBehaviorSanitizerEnabled ? std::string_view{"asan+ubsan"}
    : kAddressSanitizerEnabled                                        ? std::string_view{"asan"}
    : kUndefinedBehaviorSanitizerEnabled                              ? std::string_view{"ubsan"}
                                                                     : std::string_view{"none"};

// A stable, reproducible description of how this translation unit was compiled.
// No timestamps or machine-specific data are included so that builds are
// reproducible and byte-identical across environments.
[[nodiscard]] std::string_view build_info() noexcept;

// True when the library was compiled with assertions enabled.
[[nodiscard]] constexpr bool assertions_enabled() noexcept {
#if defined(NDEBUG)
  return false;
#else
  return true;
#endif
}

}  // namespace po
