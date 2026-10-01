// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "power_observatory/context.hpp"

namespace po {

struct QualityFinding {
  ReasonCode code{ReasonCode::None};
  Severity severity{Severity::Info};
  EntityRef entity;
  MeasurementKind kind{MeasurementKind::ActivePower};
  Phase phase{Phase::Total};
  std::string detail;
  std::optional<EvidenceRef> evidence;

  friend bool operator==(const QualityFinding&, const QualityFinding&) noexcept = default;
  friend auto operator<=>(const QualityFinding&, const QualityFinding&) noexcept = default;
};

struct QualityReport {
  EvidenceState state{EvidenceState::Known};
  Timestamp computed_at{};
  std::vector<QualityFinding> findings;
  std::size_t measurements_assessed{0};
  std::size_t measurements_skipped_stale{0};
  Explanation explanation;
};

struct QualityQuery {
  std::optional<EntityRef> scope;
};

[[nodiscard]] Outcome<QualityReport> compute_quality(const ObservationContext& context, const QualityQuery& query);

}  // namespace po
