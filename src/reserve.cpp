// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/reserve.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace po {
namespace {

// The set of elements whose declared capacity supplies the scope. Reserve is
// always computed over supply components, never over arbitrary entities, so a
// PDU or a circuit can never contribute capacity it does not have.
[[nodiscard]] std::vector<EntityRef> supply_components(const ObservationContext& context, const EntityRef& scope) {
  std::vector<EntityRef> components;

  if (scope.kind() == EntityKind::RedundancyGroup) {
    const RedundancyGroup* group = context.topology.redundancy_group(RedundancyGroupId(scope.id()));
    if (group != nullptr) {
      for (const FeedId& member : group->members) {
        components.push_back(EntityRef::feed(member));
      }
    }
  } else if (scope.kind() == EntityKind::Feed) {
    components.push_back(scope);
  } else if (scope.kind() == EntityKind::Ups || scope.kind() == EntityKind::Generator) {
    components.push_back(scope);
    for (const EntityRef& upstream : context.topology.upstream_feeds(scope)) {
      components.push_back(upstream);
    }
  } else {
    for (const EntityRef& upstream : context.topology.upstream_feeds(scope)) {
      components.push_back(upstream);
    }
    if (components.empty()) {
      // Nothing upstream was declared. Fall back to the scope itself so that a
      // generator or an islanded bus still produces an answer rather than a
      // silent zero.
      components.push_back(scope);
    }
  }

  std::sort(components.begin(), components.end());
  components.erase(std::unique(components.begin(), components.end()), components.end());
  return components;
}

[[nodiscard]] std::string quantity_text(Power value) { return value.to_string(); }

}  // namespace

Outcome<ReserveReport> compute_reserve(const ObservationContext& context, const ReserveQuery& query) {
  ReserveReport report;
  Explanation explanation;
  report.scope = query.scope;

  if (!context.topology.contains(query.scope)) {
    explanation.add(ReasonCode::UnknownEntity, query.scope.to_string(),
                    "the declared topology contains no such entity");
    return Outcome<ReserveReport>(
        Error(ReasonCode::UnknownEntity, "reserve query names an entity that the topology does not declare"),
        std::move(explanation));
  }

  const std::vector<EntityRef> components = supply_components(context, query.scope);
  if (components.empty()) {
    explanation.add(ReasonCode::CapacityUnknown, query.scope.to_string(),
                    "no supply component could be identified for this scope");
    return Outcome<ReserveReport>(
        Error(ReasonCode::CapacityUnknown, "reserve query could not identify any supply component"),
        std::move(explanation));
  }

  Accumulator<PowerUnitTag> usable_total;
  Accumulator<PowerUnitTag> load_total;
  std::optional<Power> largest_capacity;
  bool load_complete = true;
  std::size_t missing_loads = 0;

  for (const EntityRef& component : components) {
    ReserveComponent entry;
    entry.entity = component;
    entry.declared_state = context.topology.declared_state(component);
    entry.declared_capacity = context.topology.declared_capacity(component);

    const bool available = declared_available(entry.declared_state);
    if (available) {
      const Result<Power> derated = scale_by_ppm(entry.declared_capacity, context.policy.reserve.derate_ppm.raw());
      if (!derated) {
        explanation.add(derated.code(), component.to_string(), derated.detail());
        entry.state = EvidenceState::Indeterminate;
        load_complete = false;
        ++missing_loads;
        report.components.push_back(std::move(entry));
        continue;
      }
      entry.usable_capacity = derated.value();
      usable_total.add(derated.value());
      if (!largest_capacity.has_value() || derated.value() > *largest_capacity) {
        largest_capacity = derated.value();
      }
      if (context.policy.reserve.derate_ppm.raw() != 1000000) {
        explanation.add(ReasonCode::CapacityDerated, component.to_string(),
                        "declared capacity " + quantity_text(entry.declared_capacity) + " derated to " +
                            quantity_text(derated.value()) + " by policy");
      }
    } else {
      explanation.add(ReasonCode::CapacityDerated, component.to_string(),
                      "declared state is " + std::string(po::to_string(entry.declared_state)) +
                          ", so this element contributes no usable capacity");
    }

    const MeasurementView view = select_active_power(context, component);
    if (!view.present()) {
      entry.state = EvidenceState::Unknown;
      load_complete = false;
      ++missing_loads;
      explanation.add(ReasonCode::NotMeasured, component.to_string(),
                      "no active power measurement was admitted, so the load this element carries is unknown");
    } else {
      entry.evidence.push_back(view.reference());
      const Result<Power> value = as_power(*view.measurement);
      if (!value) {
        entry.state = EvidenceState::Unsupported;
        load_complete = false;
        ++missing_loads;
        explanation.add(value.code(), component.to_string(), value.detail());
      } else if (!view.usable()) {
        entry.state = to_evidence_state(view.freshness.classification);
        load_complete = false;
        ++missing_loads;
        explanation.add(view.freshness.reason, component.to_string(),
                        "measured " + quantity_text(value.value()) + " but " + view.freshness.detail);
        if (view.freshness.anomaly.has_value()) {
          explanation.add(*view.freshness.anomaly, component.to_string(), view.freshness.anomaly_detail);
        }
      } else {
        entry.state = EvidenceState::Known;
        entry.measured_load = value.value();
        load_total.add(value.value());
        if (view.freshness.anomaly.has_value()) {
          explanation.add(*view.freshness.anomaly, component.to_string(), view.freshness.anomaly_detail);
        }
      }
    }

    if (entry.usable_capacity.has_value() && entry.measured_load.has_value()) {
      QuantityRep reserve_raw{};
      if (checked_sub(entry.usable_capacity->raw(), entry.measured_load->raw(), reserve_raw)) {
        entry.reserve = Power::from_raw(reserve_raw);
        if (entry.reserve->is_negative()) {
          explanation.add(ReasonCode::ReserveNegative, component.to_string(),
                          "measured load " + quantity_text(*entry.measured_load) +
                              " exceeds usable capacity " + quantity_text(*entry.usable_capacity));
        }
      } else {
        explanation.add(ReasonCode::ArithmeticOverflow, component.to_string(),
                        "usable capacity minus measured load overflowed 64-bit range");
      }
      if (!entry.usable_capacity->is_zero()) {
        const Result<Ratio> fraction = ratio_ppm_of(*entry.measured_load, *entry.usable_capacity);
        if (fraction) {
          entry.load_fraction_ppm = fraction.value();
        }
      }
    }

    report.components.push_back(std::move(entry));
  }

  std::sort(report.components.begin(), report.components.end());
  report.total_components = report.components.size();
  report.known_components = static_cast<std::size_t>(
      std::count_if(report.components.begin(), report.components.end(), [](const ReserveComponent& entry) {
        return entry.state == EvidenceState::Known;
      }));

  if (usable_total.overflowed()) {
    explanation.add(ReasonCode::ArithmeticOverflow, query.scope.to_string(),
                    "the sum of usable capacity overflowed 64-bit range");
  } else {
    report.usable_capacity = usable_total.saturated_total();
  }

  if (load_complete && report.usable_capacity.has_value()) {
    report.measured_load = load_total.saturated_total();
    QuantityRep reserve_raw{};
    if (checked_sub(report.usable_capacity->raw(), report.measured_load->raw(), reserve_raw)) {
      report.reserve = Power::from_raw(reserve_raw);
      if (report.reserve->is_negative()) {
        explanation.add(ReasonCode::ReserveNegative, query.scope.to_string(),
                        "measured load " + quantity_text(*report.measured_load) +
                            " exceeds usable capacity " + quantity_text(*report.usable_capacity));
      }
      if (!report.usable_capacity->is_zero()) {
        const Result<Ratio> fraction = ratio_ppm_of(*report.measured_load, *report.usable_capacity);
        if (fraction) {
          report.reserve_fraction_ppm = fraction.value();
        }
      }
      report.below_minimum_headroom = *report.reserve < context.policy.reserve.minimum_headroom;
      if (report.below_minimum_headroom) {
        explanation.add(ReasonCode::ReserveBelowFloor, query.scope.to_string(),
                        "reserve " + quantity_text(*report.reserve) + " is below the policy floor of " +
                            quantity_text(context.policy.reserve.minimum_headroom));
      }
    } else {
      explanation.add(ReasonCode::ArithmeticOverflow, query.scope.to_string(),
                      "usable capacity minus measured load overflowed 64-bit range");
    }
  } else if (report.usable_capacity.has_value()) {
    report.measured_load = load_total.saturated_total();
    explanation.add(ReasonCode::ReserveUnknown, query.scope.to_string(),
                    "reserve is not published because " + std::to_string(missing_loads) +
                        " of " + std::to_string(report.total_components) +
                        " supply components carry no usable load evidence; the partial load total " +
                        quantity_text(*report.measured_load) + " is a lower bound only");
  }

  // Single-failure headroom is computed by its own model: remove the largest
  // currently available contributor and ask whether what remains still covers
  // the load.
  if (largest_capacity.has_value() && report.usable_capacity.has_value()) {
    QuantityRep remaining{};
    if (checked_sub(report.usable_capacity->raw(), largest_capacity->raw(), remaining)) {
      const Power remaining_capacity = Power::from_raw(remaining);
      if (report.measured_load.has_value()) {
        QuantityRep headroom{};
        if (checked_sub(remaining_capacity.raw(), report.measured_load->raw(), headroom)) {
          report.single_failure_reserve = Power::from_raw(headroom);
          report.single_failure_capable = *report.single_failure_reserve >= context.policy.reserve.minimum_headroom;
          if (!report.single_failure_capable) {
            explanation.add(ReasonCode::RedundancyLost, query.scope.to_string(),
                            "after losing the largest available contributor " + quantity_text(*largest_capacity) +
                                ", headroom is " + quantity_text(*report.single_failure_reserve) +
                                ", below the policy floor of " +
                                quantity_text(context.policy.reserve.minimum_headroom));
          }
        } else {
          explanation.add(ReasonCode::ArithmeticOverflow, query.scope.to_string(),
                          "single-failure headroom overflowed 64-bit range");
        }
      }
    }
  }

  // Independent cross-check against the load measured downstream.
  const LoadEstimate downstream = estimate_load(context, query.scope, false);
  if (downstream.total.has_value()) {
    report.downstream_measured_load = downstream.total;
    if (report.measured_load.has_value() && !report.measured_load->is_zero()) {
      QuantityRep delta{};
      if (checked_sub(report.measured_load->raw(), downstream.total->raw(), delta)) {
        const Result<Ratio> share = ratio_ppm_of(Power::from_raw(delta), *report.measured_load);
        if (share) {
          report.supply_downstream_delta_ppm = share.value();
        }
        const QuantityRep magnitude = delta < 0 ? (~delta + 1) : delta;
        const QuantityRep tolerance_raw =
            report.measured_load->raw() / 1000000 * context.policy.imbalance.tolerance_ppm.raw();
        if (magnitude > tolerance_raw) {
          explanation.add(ReasonCode::SourceDisagreement, query.scope.to_string(),
                          "supply-side meters total " + quantity_text(*report.measured_load) +
                              " while downstream meters total " + quantity_text(*downstream.total) +
                              "; the two views of the same load disagree");
        }
      }
    }
    if (!downstream.complete()) {
      explanation.add(ReasonCode::MissingMeasurement, query.scope.to_string(),
                      std::to_string(downstream.unmeasured.size()) +
                          " downstream entit" + (downstream.unmeasured.size() == 1 ? "y" : "ies") +
                          " carry no usable measurement, so the downstream cross-check is partial");
    }
  } else {
    explanation.add(ReasonCode::NotMeasured, query.scope.to_string(),
                    "no downstream load could be measured, so the supply-side total could not be cross-checked");
  }

  if (!report.reserve.has_value()) {
    report.state = missing_loads > 0 && report.known_components == 0 ? EvidenceState::Indeterminate
                                                                    : EvidenceState::Indeterminate;
    if (report.known_components > 0) {
      report.state = EvidenceState::Indeterminate;
    } else {
      report.state = EvidenceState::Unknown;
    }
  } else if (report.known_components == report.total_components) {
    report.state = EvidenceState::Known;
    explanation.add(ReasonCode::ReserveAdequate, query.scope.to_string(),
                    "reserve " + quantity_text(*report.reserve) + " against usable capacity " +
                        quantity_text(*report.usable_capacity));
  } else {
    report.state = EvidenceState::Stale;
  }

  explanation.canonicalize();
  report.explanation = explanation;
  return Outcome<ReserveReport>(std::move(report), std::move(explanation));
}

}  // namespace po
