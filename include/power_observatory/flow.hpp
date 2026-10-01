// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "power_observatory/context.hpp"

namespace po {

// Direction of active power at an entity, as measured. Positive measured active
// power means power flowing into the entity from its upstream side.
enum class FlowDirection : std::uint8_t {
  Unknown = 0,
  Importing = 1,
  Exporting = 2,
  Idle = 3,
};

[[nodiscard]] std::string_view to_string(FlowDirection direction) noexcept;

struct FlowNode {
  EntityRef entity;
  EvidenceState state{EvidenceState::Unknown};
  FreshnessClass freshness{FreshnessClass::Unknown};
  std::optional<Power> active_power;
  // True when this node's measurement contributed to the totals. A value can be
  // reported and still not be counted, and this flag is what says which.
  bool counted{false};
  FlowDirection direction{FlowDirection::Unknown};
  // Share of the total observed load, when a total could be established.
  std::optional<Ratio> share_of_total_ppm;
  // Measured parent minus the sum of its measured children. Present whenever
  // both sides could be established, whether or not the residual is a finding.
  std::optional<Power> child_residual;
  std::optional<Ratio> child_residual_ppm;
  std::vector<EntityRef> measured_children;
  std::vector<EntityRef> unmeasured_children;
  std::vector<EvidenceRef> evidence;

  friend bool operator==(const FlowNode&, const FlowNode&) noexcept = default;
  friend auto operator<=>(const FlowNode&, const FlowNode&) noexcept = default;
};

struct FlowReport {
  EvidenceState state{EvidenceState::Unknown};
  Timestamp computed_at{};
  std::vector<FlowNode> nodes;
  std::optional<Power> total_observed_load;
  std::size_t measured_nodes{0};
  std::size_t unmeasured_nodes{0};
  Explanation explanation;
};

struct FlowQuery {
  // Empty scope means the whole declared model.
  std::optional<EntityRef> scope;
  bool include_children{true};
};

[[nodiscard]] Outcome<FlowReport> compute_flow(const ObservationContext& context, const FlowQuery& query);

}  // namespace po
