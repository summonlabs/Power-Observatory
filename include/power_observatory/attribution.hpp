// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "power_observatory/context.hpp"

namespace po {

// Residual between a parent's measured active power and the sum of its measured
// children. The residual is what is left over for branches this runtime cannot
// see, and it is reported as a number rather than explained away.
struct ImbalanceRecord {
  EntityRef parent;
  EvidenceState state{EvidenceState::Unknown};
  Power parent_measured{};
  Power children_total{};
  Power residual{};
  std::optional<Ratio> residual_ppm;
  Ratio tolerance_ppm{};
  Power absolute_floor{};
  bool within_tolerance{true};
  std::vector<EntityRef> measured_children;
  std::vector<EntityRef> unmeasured_children;
  std::vector<EvidenceRef> evidence;

  friend bool operator==(const ImbalanceRecord&, const ImbalanceRecord&) noexcept = default;
  friend auto operator<=>(const ImbalanceRecord&, const ImbalanceRecord&) noexcept = default;
};

// Conversion loss across a node whose input and output are both measured. Loss
// is never inferred from declared efficiency; when only the declared efficiency
// exists, the record says so and its state is Unsupported.
struct LossRecord {
  EntityRef node;
  EvidenceState state{EvidenceState::Unknown};
  Power input{};
  Power output{};
  Power loss{};
  std::optional<Ratio> loss_ppm;
  Ratio limit_ppm{};
  Power absolute_floor{};
  bool within_limit{true};
  std::optional<Ratio> declared_efficiency_ppm;
  std::vector<EvidenceRef> evidence;

  friend bool operator==(const LossRecord&, const LossRecord&) noexcept = default;
  friend auto operator<=>(const LossRecord&, const LossRecord&) noexcept = default;
};

struct AttributionReport {
  EvidenceState state{EvidenceState::Unknown};
  Timestamp computed_at{};
  std::vector<ImbalanceRecord> imbalances;
  std::vector<LossRecord> losses;
  // Sum of the losses that could actually be measured. Absent when no loss
  // could be measured at all, so that a missing measurement is never rendered
  // as a zero.
  std::optional<Power> total_measured_loss;
  std::size_t unattributed_imbalances{0};
  Explanation explanation;
};

struct AttributionQuery {
  std::optional<EntityRef> scope;
  bool include_imbalance{true};
  bool include_losses{true};
};

[[nodiscard]] Outcome<AttributionReport> compute_attribution(const ObservationContext& context,
                                                             const AttributionQuery& query);

}  // namespace po
