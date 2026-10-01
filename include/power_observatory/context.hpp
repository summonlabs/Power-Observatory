// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <optional>

#include "power_observatory/evidence.hpp"
#include "power_observatory/explanation.hpp"
#include "power_observatory/freshness.hpp"
#include "power_observatory/policy.hpp"
#include "power_observatory/topology.hpp"

namespace po {

// Everything the analysis engines need, assembled once per query. The context
// owns no state: it binds an immutable evidence set, an immutable topology, a
// policy, and the two clock readings taken when the query started. Every engine
// therefore sees exactly the same notion of "now", which is what makes two runs
// of the same query reproducible.
struct ObservationContext {
  const EvidenceSet& evidence;
  const TopologyModel& topology;
  const FreshnessModel& freshness;
  const ObservationPolicy& policy;
  Timestamp wall_now;
  MonotonicInstant steady_now;
};

// A measurement together with the freshness decision that applies to it.
struct MeasurementView {
  const Measurement* measurement{nullptr};
  FreshnessAssessment freshness{};

  [[nodiscard]] bool present() const noexcept { return measurement != nullptr; }
  [[nodiscard]] bool usable() const noexcept { return measurement != nullptr && freshness.usable(); }
  [[nodiscard]] EvidenceRef reference() const {
    return measurement == nullptr ? EvidenceRef{} : make_ref(*measurement);
  }
};

// The most trustworthy measurement for an entity/kind/phase. Trustworthiness is
// ordered by freshness class first, then by generation and sequence, so a
// fresher reading always wins over a newer-but-stale one.
[[nodiscard]] MeasurementView select_measurement(const ObservationContext& context, const EntityRef& entity,
                                                 MeasurementKind kind, Phase phase = Phase::Total);

// Convenience for the overwhelmingly common case.
[[nodiscard]] MeasurementView select_active_power(const ObservationContext& context, const EntityRef& entity);

// Extracts an active-power value, refusing when the measurement is of another
// kind. Present so that no engine reaches into the variant directly.
[[nodiscard]] Result<Power> as_power(const Measurement& measurement);

// A load estimate derived from the declared wiring.
//
// The traversal descends from the scope and counts an entity only when it is
// metered and none of its children are metered, so a metered parent is never
// added to a metered child. Entities that carry no usable measurement are
// listed rather than assumed to be zero.
struct LoadEstimate {
  EntityRef scope;
  std::optional<Power> total;
  std::vector<EntityRef> counted;
  std::vector<EntityRef> unmeasured;
  std::vector<EvidenceRef> evidence;

  [[nodiscard]] bool complete() const noexcept { return unmeasured.empty(); }
};

[[nodiscard]] LoadEstimate estimate_load(const ObservationContext& context, const EntityRef& scope,
                                         bool include_scope);

}  // namespace po
