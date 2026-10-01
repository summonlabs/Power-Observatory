// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/reason.hpp"

#include <array>
#include <cstddef>
#include <iterator>
#include <string>

namespace po {
namespace {

struct ReasonEntry {
  ReasonCode code;
  std::string_view enumerator;
  Severity severity;
};

constexpr ReasonEntry kReasonEntries[] = {
#define PO_REASON_ENTRY(name, value, severity) {ReasonCode::name, #name, Severity::severity},
    PO_REASON_CODES(PO_REASON_ENTRY)
#undef PO_REASON_ENTRY
};

constexpr std::size_t kReasonCount = std::size(kReasonEntries);

// Converts an enumerator spelling such as "SplitBrainEvidence" to the wire form
// "split_brain_evidence". Deriving the wire name from the enumerator keeps the
// two from ever drifting apart.
std::string to_snake_case(std::string_view text) {
  std::string result;
  result.reserve(text.size() + 8);
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char current = text[index];
    const bool upper = current >= 'A' && current <= 'Z';
    if (upper && index > 0) {
      const char previous = text[index - 1];
      const bool previous_lower = previous >= 'a' && previous <= 'z';
      const bool previous_digit = previous >= '0' && previous <= '9';
      const bool next_lower = index + 1 < text.size() && text[index + 1] >= 'a' && text[index + 1] <= 'z';
      const bool previous_upper = previous >= 'A' && previous <= 'Z';
      if (previous_lower || previous_digit || (previous_upper && next_lower)) {
        result.push_back('_');
      }
    }
    result.push_back(static_cast<char>(upper ? (current - 'A' + 'a') : current));
  }
  return result;
}

struct ReasonNames {
  std::array<std::string, kReasonCount> names;
  ReasonNames() {
    for (std::size_t index = 0; index < kReasonCount; ++index) {
      names[index] = to_snake_case(kReasonEntries[index].enumerator);
    }
  }
};

const ReasonNames& names() {
  static const ReasonNames table;
  return table;
}

}  // namespace

std::string_view to_string(Severity severity) noexcept {
  switch (severity) {
    case Severity::Info:
      return "info";
    case Severity::Notice:
      return "notice";
    case Severity::Warning:
      return "warning";
    case Severity::Error:
      return "error";
    case Severity::Critical:
      return "critical";
  }
  return "unknown";
}

std::string_view to_string(ReasonCode code) noexcept {
  for (std::size_t index = 0; index < kReasonCount; ++index) {
    if (kReasonEntries[index].code == code) {
      return names().names[index];
    }
  }
  return "unrecognized_reason";
}

Severity severity(ReasonCode code) noexcept {
  for (const ReasonEntry& entry : kReasonEntries) {
    if (entry.code == code) {
      return entry.severity;
    }
  }
  return Severity::Critical;
}

ReasonCategory category(ReasonCode code) noexcept {
  const auto value = static_cast<std::uint16_t>(code);
  if (value == 0) {
    return ReasonCategory::None;
  }
  if (value < 200) {
    return ReasonCategory::Informational;
  }
  if (value < 300) {
    return ReasonCategory::Unknown;
  }
  if (value < 400) {
    return ReasonCategory::Freshness;
  }
  if (value < 500) {
    return ReasonCategory::Conflict;
  }
  if (value < 600) {
    return ReasonCategory::Numeric;
  }
  if (value < 700) {
    return ReasonCategory::Reserve;
  }
  if (value < 800) {
    return ReasonCategory::Failover;
  }
  if (value < 900) {
    return ReasonCategory::Authority;
  }
  if (value < 1000) {
    return ReasonCategory::Persistence;
  }
  if (value < 1100) {
    return ReasonCategory::Lifecycle;
  }
  return ReasonCategory::Parsing;
}

std::string_view to_string(ReasonCategory category) noexcept {
  switch (category) {
    case ReasonCategory::None:
      return "none";
    case ReasonCategory::Informational:
      return "informational";
    case ReasonCategory::Unknown:
      return "unknown";
    case ReasonCategory::Freshness:
      return "freshness";
    case ReasonCategory::Conflict:
      return "conflict";
    case ReasonCategory::Numeric:
      return "numeric";
    case ReasonCategory::Reserve:
      return "reserve";
    case ReasonCategory::Failover:
      return "failover";
    case ReasonCategory::Authority:
      return "authority";
    case ReasonCategory::Persistence:
      return "persistence";
    case ReasonCategory::Lifecycle:
      return "lifecycle";
    case ReasonCategory::Parsing:
      return "parsing";
  }
  return "unknown";
}

std::size_t reason_code_count() noexcept { return kReasonCount; }

ReasonCode reason_code_at(std::size_t index) noexcept { return kReasonEntries[index].code; }

}  // namespace po
