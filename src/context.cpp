// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/context.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace po {
namespace {

// Bounds any traversal so that a malformed or unexpectedly deep model still
// terminates. The bound is derived from the model size, not from a clock.
[[nodiscard]] std::size_t traversal_budget(const TopologyModel& topology) noexcept {
  return (topology.size() + 1) * 4;
}

}  // namespace

MeasurementView select_measurement(const ObservationContext& context, const EntityRef& entity, MeasurementKind kind,
                                   Phase phase) {
  MeasurementView best;
  for (const Measurement* candidate : context.evidence.for_entity(entity, kind)) {
    if (candidate->phase != phase) {
      continue;
    }
    const FreshnessAssessment freshness = context.freshness.assess(*candidate, context.wall_now, context.steady_now);
    if (!best.present()) {
      best.measurement = candidate;
      best.freshness = freshness;
      continue;
    }
    const int candidate_rank = FreshnessModel::rank(freshness.classification);
    const int best_rank = FreshnessModel::rank(best.freshness.classification);
    if (candidate_rank != best_rank) {
      if (candidate_rank < best_rank) {
        best.measurement = candidate;
        best.freshness = freshness;
      }
      continue;
    }
    if (std::tie(candidate->provenance.generation, candidate->provenance.sequence, candidate->id) >
        std::tie(best.measurement->provenance.generation, best.measurement->provenance.sequence,
                 best.measurement->id)) {
      best.measurement = candidate;
      best.freshness = freshness;
    }
  }
  return best;
}

MeasurementView select_active_power(const ObservationContext& context, const EntityRef& entity) {
  return select_measurement(context, entity, MeasurementKind::ActivePower, Phase::Total);
}

Result<Power> as_power(const Measurement& measurement) {
  if (measurement.kind() != MeasurementKind::ActivePower) {
    return Error(ReasonCode::UnsupportedQuantity,
                 measurement.entity.to_string() + " carries " +
                     std::string(po::to_string(measurement.kind())) + " where active power was required");
  }
  return std::get<Power>(measurement.value);
}

LoadEstimate estimate_load(const ObservationContext& context, const EntityRef& scope, bool include_scope) {
  LoadEstimate estimate;
  estimate.scope = scope;

  std::vector<EntityRef> pending;
  if (include_scope) {
    pending.push_back(scope);
  } else {
    for (const EntityRef& child : context.topology.children(scope)) {
      pending.push_back(child);
    }
  }

  std::set<EntityRef> visited;
  Accumulator<PowerUnitTag> accumulator;
  const std::size_t budget = traversal_budget(context.topology);
  std::size_t steps = 0;

  while (!pending.empty() && steps < budget) {
    ++steps;
    const EntityRef current = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }

    const MeasurementView view = select_active_power(context, current);
    const std::vector<EntityRef> children = context.topology.children(current);

    std::vector<EntityRef> metered_children;
    std::vector<EntityRef> unmetered_children;
    for (const EntityRef& child : children) {
      const MeasurementView child_view = select_active_power(context, child);
      if (child_view.usable()) {
        metered_children.push_back(child);
      } else {
        unmetered_children.push_back(child);
      }
    }

    for (const EntityRef& child : unmetered_children) {
      estimate.unmeasured.push_back(child);
    }

    if (view.usable()) {
      if (metered_children.empty()) {
        const Result<Power> value = as_power(*view.measurement);
        if (value) {
          accumulator.add(value.value());
          estimate.counted.push_back(current);
          estimate.evidence.push_back(view.reference());
        }
      } else {
        for (const EntityRef& child : metered_children) {
          pending.push_back(child);
        }
      }
      continue;
    }

    if (children.empty()) {
      estimate.unmeasured.push_back(current);
      continue;
    }
    for (const EntityRef& child : children) {
      pending.push_back(child);
    }
  }

  if (!estimate.counted.empty()) {
    estimate.total = accumulator.overflowed() ? std::nullopt : std::optional<Power>(accumulator.saturated_total());
  }

  std::sort(estimate.counted.begin(), estimate.counted.end());
  estimate.counted.erase(std::unique(estimate.counted.begin(), estimate.counted.end()), estimate.counted.end());
  std::sort(estimate.unmeasured.begin(), estimate.unmeasured.end());
  estimate.unmeasured.erase(std::unique(estimate.unmeasured.begin(), estimate.unmeasured.end()),
                            estimate.unmeasured.end());
  std::sort(estimate.evidence.begin(), estimate.evidence.end());
  estimate.evidence.erase(std::unique(estimate.evidence.begin(), estimate.evidence.end()), estimate.evidence.end());
  return estimate;
}

}  // namespace po
