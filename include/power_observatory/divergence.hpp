// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "power_observatory/evidence.hpp"
#include "power_observatory/explanation.hpp"
#include "power_observatory/snapshot.hpp"

namespace po {

enum class DivergenceClass : std::uint8_t {
  None = 0,
  Appeared = 1,
  Disappeared = 2,
  ValueChanged = 3,
  SignFlip = 4,
  GenerationAdvanced = 5,
  GenerationRegressed = 6,
  AuthorityChanged = 7,
  OriginChanged = 8,
};

[[nodiscard]] std::string_view to_string(DivergenceClass classification) noexcept;

struct DivergenceEntry {
  EntityRef entity;
  MeasurementKind kind{MeasurementKind::ActivePower};
  Phase phase{Phase::Total};
  DivergenceClass classification{DivergenceClass::None};
  std::optional<QuantityRep> left_raw;
  std::optional<QuantityRep> right_raw;
  std::optional<Generation> left_generation;
  std::optional<Generation> right_generation;
  std::string detail;

  friend bool operator==(const DivergenceEntry&, const DivergenceEntry&) noexcept = default;
  friend auto operator<=>(const DivergenceEntry&, const DivergenceEntry&) noexcept = default;
};

// How two evidence generations relate. Divergence is reported, never resolved:
// this runtime does not decide which of two disagreeing authorities is right.
enum class DivergenceVerdict : std::uint8_t {
  Identical = 0,
  Advanced = 1,
  Regressed = 2,
  Diverged = 3,
};

[[nodiscard]] std::string_view to_string(DivergenceVerdict verdict) noexcept;

struct DivergenceReport {
  DivergenceVerdict verdict{DivergenceVerdict::Identical};
  GenerationOrder order{GenerationOrder::Equal};
  std::size_t compared_keys{0};
  std::size_t diverged_keys{0};
  std::size_t appeared{0};
  std::size_t disappeared{0};
  std::size_t value_changes{0};
  std::size_t generation_changes{0};
  std::vector<DivergenceEntry> entries;
  Explanation explanation;
};

[[nodiscard]] DivergenceReport compare(const Snapshot& left, const Snapshot& right);
[[nodiscard]] DivergenceReport compare(const EvidenceSet& left, const EvidenceSet& right);

}  // namespace po
