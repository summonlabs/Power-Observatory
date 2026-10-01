// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "power_observatory/context.hpp"

namespace po {

struct ReserveComponent {
  EntityRef entity;
  EvidenceState state{EvidenceState::Unknown};
  LifecycleState declared_state{LifecycleState::Unknown};
  Power declared_capacity{};
  // Declared capacity after the policy derate. Present only when the element is
  // declared available; a missing value means the element contributes nothing
  // and cannot be counted on.
  std::optional<Power> usable_capacity;
  std::optional<Power> measured_load;
  std::optional<Power> reserve;
  std::optional<Ratio> load_fraction_ppm;
  std::vector<EvidenceRef> evidence;

  friend bool operator==(const ReserveComponent&, const ReserveComponent&) noexcept = default;
  friend auto operator<=>(const ReserveComponent&, const ReserveComponent&) noexcept = default;
};

struct ReserveReport {
  EvidenceState state{EvidenceState::Unknown};
  EntityRef scope;
  std::optional<Power> usable_capacity;
  std::optional<Power> measured_load;
  std::optional<Power> reserve;
  std::optional<Ratio> reserve_fraction_ppm;
  // Headroom once the largest currently available contributor is removed. This
  // is the quantity that decides whether the remaining capacity can absorb a
  // transfer, and it is computed by a model that never consults the plain
  // reserve total.
  std::optional<Power> single_failure_reserve;
  bool single_failure_capable{false};
  bool below_minimum_headroom{false};
  // Independent cross-check of the load: what the declared wiring says is being
  // drawn downstream of the scope. When the supply-side meters and the
  // downstream meters disagree beyond the imbalance tolerance, the
  // disagreement is reported rather than averaged away.
  std::optional<Power> downstream_measured_load;
  std::optional<Ratio> supply_downstream_delta_ppm;
  std::size_t known_components{0};
  std::size_t total_components{0};
  std::vector<ReserveComponent> components;
  Explanation explanation;
};

struct ReserveQuery {
  EntityRef scope;
  bool include_children{true};
};

[[nodiscard]] Outcome<ReserveReport> compute_reserve(const ObservationContext& context, const ReserveQuery& query);

}  // namespace po
