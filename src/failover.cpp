// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/failover.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace po {
namespace {

// The weakest declared ride-through below an entity. Configuration is used only
// to locate the equipment; the number that comes back is explicitly a declared
// autonomy, and a gate built on it says so.
[[nodiscard]] std::optional<Duration> declared_autonomy_below(const ObservationContext& context,
                                                              const EntityRef& origin) {
  std::optional<Duration> weakest;
  std::vector<EntityRef> pending{origin};
  std::set<EntityRef> visited;
  const std::size_t budget = (context.topology.size() + 1) * 4;
  std::size_t steps = 0;

  while (!pending.empty() && steps < budget) {
    ++steps;
    const EntityRef current = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }

    std::optional<Duration> declared;
    if (current.kind() == EntityKind::Ups) {
      const Ups* unit = context.topology.ups(UpsId(current.id()));
      if (unit != nullptr) {
        declared = unit->declared_autonomy;
      }
    } else if (current.kind() == EntityKind::Generator) {
      const Generator* unit = context.topology.generator(GeneratorId(current.id()));
      if (unit != nullptr) {
        declared = unit->declared_autonomy;
      }
    }
    if (declared.has_value() && declared->raw() > 0) {
      if (!weakest.has_value() || *declared < *weakest) {
        weakest = declared;
      }
    }

    for (const EntityRef& child : context.topology.children(current)) {
      pending.push_back(child);
    }
  }
  return weakest;
}

[[nodiscard]] std::string capacity_text(Power value) { return value.to_string(); }

}  // namespace

std::string_view to_string(FailoverReadiness readiness) noexcept {
  switch (readiness) {
    case FailoverReadiness::Unknown:
      return "unknown";
    case FailoverReadiness::Ready:
      return "ready";
    case FailoverReadiness::Degraded:
      return "degraded";
    case FailoverReadiness::NotReady:
      return "not_ready";
    case FailoverReadiness::Unsupported:
      return "unsupported";
  }
  return "unknown";
}

Outcome<FailoverReport> compute_failover(const ObservationContext& context, const FailoverQuery& query) {
  FailoverReport report;
  Explanation explanation;
  report.group = query.group;

  const RedundancyGroup* group = context.topology.redundancy_group(query.group);
  if (group == nullptr) {
    explanation.add(ReasonCode::UnknownEntity, EntityRef::redundancy_group(query.group).to_string(),
                    "the declared topology contains no such redundancy group");
    return Outcome<FailoverReport>(
        Error(ReasonCode::UnknownEntity, "failover query names a redundancy group the topology does not declare"),
        std::move(explanation));
  }

  report.topology = group->topology;
  report.required_live = group->required_live;
  const std::string subject = EntityRef::redundancy_group(group->id).to_string();

  // --- members declared -----------------------------------------------------
  {
    FailoverGate gate;
    gate.gate = "members_declared";
    gate.mandatory = true;
    gate.evaluable = true;
    gate.passed = group->members.size() >= group->required_live;
    gate.code = gate.passed ? ReasonCode::Ok : ReasonCode::RedundancyLost;
    gate.detail = "group topology " + std::string(po::to_string(group->topology)) + " declares " +
                  std::to_string(group->members.size()) + " member feed(s) and requires " +
                  std::to_string(group->required_live) + " to be live";
    if (gate.passed) {
      explanation.add(gate.code, subject, gate.detail);
    } else {
      explanation.add(gate.code, subject, gate.detail);
    }
    report.gates.push_back(std::move(gate));
  }

  // --- live membership ------------------------------------------------------
  std::vector<EntityRef> live;
  std::vector<EntityRef> dead;
  std::vector<EntityRef> live_with_load;
  std::optional<Power> largest_live_load;
  Power live_capacity_sum{};
  Power largest_live_capacity{};
  bool capacity_overflow = false;
  bool all_live_fresh = true;
  bool any_live_aging = false;

  Accumulator<PowerUnitTag> capacity_accumulator;
  for (const FeedId& member : group->members) {
    const EntityRef entity = EntityRef::feed(member);
    const MeasurementView view = select_active_power(context, entity);
    const LifecycleState declared = context.topology.declared_state(entity);
    const Power declared_capacity = context.topology.declared_capacity(entity);

    bool is_live = false;
    if (declared_available(declared) && view.usable()) {
      const Result<Power> value = as_power(*view.measurement);
      if (value && value.value().is_positive()) {
        is_live = true;
      }
    }

    if (!is_live) {
      dead.push_back(entity);
      if (!view.present()) {
        explanation.add(ReasonCode::NotMeasured, entity.to_string(),
                        "no active power measurement, so this member cannot be counted as live");
      } else if (!view.usable()) {
        explanation.add(view.freshness.reason, entity.to_string(),
                        "evidence is present but " + view.freshness.detail);
      } else {
        const Result<Power> value = as_power(*view.measurement);
        if (value) {
          explanation.add(value.value().is_positive() ? ReasonCode::CapacityDerated : ReasonCode::PeerPathDead,
                          entity.to_string(),
                          "measured " + value.value().to_string() + " with declared state " +
                              std::string(po::to_string(declared)));
        }
      }
      continue;
    }

    live.push_back(entity);
    if (view.freshness.classification != FreshnessClass::Fresh) {
      all_live_fresh = false;
      any_live_aging = true;
      explanation.add(ReasonCode::AgingEvidence, entity.to_string(),
                      "live member evidence is " +
                          std::string(po::to_string(view.freshness.classification)) + ", not fresh");
    }
    if (view.freshness.anomaly.has_value()) {
      explanation.add(*view.freshness.anomaly, entity.to_string(), view.freshness.anomaly_detail);
    }

    const Result<Power> derated = scale_by_ppm(declared_capacity, context.policy.reserve.derate_ppm.raw());
    if (derated) {
      capacity_accumulator.add(derated.value());
      if (derated.value() > largest_live_capacity) {
        largest_live_capacity = derated.value();
      }
    } else {
      capacity_overflow = true;
      explanation.add(derated.code(), entity.to_string(), derated.detail());
    }

    const Result<Power> load = as_power(*view.measurement);
    if (load) {
      live_with_load.push_back(entity);
      if (!largest_live_load.has_value() || load.value() > *largest_live_load) {
        largest_live_load = load.value();
      }
    }
  }

  capacity_overflow = capacity_overflow || capacity_accumulator.overflowed();
  if (!capacity_overflow) {
    live_capacity_sum = capacity_accumulator.saturated_total();
  }
  report.currently_live = static_cast<std::uint32_t>(live.size());

  // --- gate: peer live ------------------------------------------------------
  {
    FailoverGate gate;
    gate.gate = "peer_live";
    gate.mandatory = context.policy.failover.require_peer_live;
    gate.evaluable = true;
    gate.passed = live.size() >= group->required_live;
    gate.code = gate.passed ? ReasonCode::Ok : ReasonCode::PeerPathDead;
    gate.detail = std::to_string(live.size()) + " of " + std::to_string(group->members.size()) +
                  " member feed(s) carry fresh, positive measured power; " + std::to_string(group->required_live) +
                  " required";
    if (!gate.passed) {
      explanation.add(gate.code, subject, gate.detail);
    }
    for (const EntityRef& entity : live) {
      const MeasurementView view = select_active_power(context, entity);
      if (view.present()) {
        gate.evidence.push_back(view.reference());
      }
    }
    for (const EntityRef& entity : dead) {
      const MeasurementView view = select_active_power(context, entity);
      if (view.present()) {
        gate.evidence.push_back(view.reference());
      }
    }
    report.gates.push_back(std::move(gate));
  }

  // The load that a single member failure would move elsewhere. It is the
  // largest measured load on any live member, not an average.
  if (largest_live_load.has_value()) {
    report.load_to_transfer = largest_live_load;
  }

  // --- gate: single-failure headroom ---------------------------------------
  {
    FailoverGate gate;
    gate.gate = "peer_headroom_after_single_failure";
    gate.mandatory = true;

    if (!report.load_to_transfer.has_value() || capacity_overflow ||
        live_capacity_sum < largest_live_capacity) {
      gate.evaluable = false;
      gate.code = ReasonCode::ReserveUnknown;
      gate.detail = capacity_overflow
                        ? "the sum of live member capacity overflowed 64-bit range"
                        : "no live member carries a usable measured load, so there is nothing to transfer";
      if (gate.detail.empty()) {
        gate.detail = "live member capacity could not be established";
      }
      explanation.add(gate.code, subject, gate.detail);
      report.gates.push_back(std::move(gate));
    } else {
      const Power remaining = Power::from_raw(live_capacity_sum.raw() - largest_live_capacity.raw());
      QuantityRep required_raw{};
      const QuantityRep margin = context.policy.failover.minimum_peer_margin_ppm.raw();
      if (!checked_mul_div(report.load_to_transfer->raw(), 1000000 + margin, 1000000, required_raw)) {
        gate.evaluable = false;
        gate.code = ReasonCode::ArithmeticOverflow;
        gate.detail = "the required peer capacity overflowed 64-bit range";
      } else {
        const Power required = Power::from_raw(required_raw);
        QuantityRep headroom{};
        if (!checked_sub(remaining.raw(), report.load_to_transfer->raw(), headroom)) {
          gate.evaluable = false;
          gate.code = ReasonCode::ArithmeticOverflow;
          gate.detail = "peer headroom overflowed 64-bit range";
        } else {
          report.peer_headroom_after_single_failure = Power::from_raw(headroom);
          gate.evaluable = true;
          gate.passed = remaining >= required;
          gate.code = gate.passed ? ReasonCode::Ok : ReasonCode::LoadExceedsCapacity;
          gate.detail = "after losing the largest live member (" + capacity_text(largest_live_capacity) +
                        "), " + capacity_text(remaining) + " remains against a required " +
                        capacity_text(required) + " (transfer " + capacity_text(*report.load_to_transfer) +
                        " plus a margin of " + context.policy.failover.minimum_peer_margin_ppm.to_string() + ")";
        }
      }
      for (const EntityRef& entity : live_with_load) {
        gate.evidence.push_back(select_active_power(context, entity).reference());
      }
      if (!gate.passed) {
        explanation.add(gate.code, subject, gate.detail);
      }
      report.gates.push_back(std::move(gate));
    }
  }

  // --- gate: peer frequency agreement --------------------------------------
  // Phase-angle synchronism is not a quantity this runtime carries, so the only
  // synchronism evidence available is agreement between measured source
  // frequencies. When no such evidence exists the gate is not evaluable, and
  // that is reported as an absence of evidence rather than as a pass.
  {
    FailoverGate gate;
    gate.gate = "peer_frequency_agreement";
    gate.mandatory = context.policy.failover.require_sync_evidence;

    std::vector<Frequency> frequencies;
    for (const EntityRef& entity : live) {
      const MeasurementView view = select_measurement(context, entity, MeasurementKind::Frequency, Phase::Total);
      if (!view.usable()) {
        continue;
      }
      frequencies.push_back(std::get<Frequency>(view.measurement->value));
      gate.evidence.push_back(view.reference());
    }

    if (frequencies.size() < 2) {
      gate.evaluable = false;
      gate.code = ReasonCode::SyncNotEstablished;
      gate.detail = "fewer than two live members publish a usable frequency measurement, so source synchronism "
                    "cannot be established; phase-angle synchronism is not carried by any evidence channel in this "
                    "runtime";
    } else {
      const Frequency lowest = *std::min_element(frequencies.begin(), frequencies.end());
      const Frequency highest = *std::max_element(frequencies.begin(), frequencies.end());
      QuantityRep spread{};
      if (!checked_sub(highest.raw(), lowest.raw(), spread)) {
        gate.evaluable = false;
        gate.code = ReasonCode::ArithmeticOverflow;
        gate.detail = "the frequency spread overflowed 64-bit range";
      } else {
        gate.evaluable = true;
        gate.passed = spread <= context.policy.quality.frequency_tolerance.raw();
        gate.code = gate.passed ? ReasonCode::Ok : ReasonCode::SyncNotEstablished;
        gate.detail = "measured source frequencies span " + Frequency::from_raw(spread).to_string() +
                      " against a tolerance of " +
                      context.policy.quality.frequency_tolerance.to_string();
      }
    }
    if (!gate.passed) {
      explanation.add(gate.code, subject, gate.detail);
    }
    report.gates.push_back(std::move(gate));
  }

  // --- gate: declared transfer window --------------------------------------
  {
    FailoverGate gate;
    gate.gate = "transfer_window";
    if (group->declared_transfer_time.raw() <= 0) {
      gate.mandatory = false;
      gate.evaluable = false;
      gate.code = ReasonCode::UnsupportedCapability;
      gate.detail = "the group does not declare a transfer time; a transfer time is configuration and none was "
                    "supplied, so this gate cannot be decided";
    } else {
      gate.mandatory = true;
      gate.evaluable = true;
      gate.passed = group->declared_transfer_time <= context.policy.failover.max_transfer_time;
      gate.code = gate.passed ? ReasonCode::Ok : ReasonCode::TransferWindowExceeded;
      gate.detail = "declared transfer time " + to_compact_string(group->declared_transfer_time) +
                    " against the policy window of " +
                    to_compact_string(context.policy.failover.max_transfer_time) +
                    "; this is configured data, not a measured transfer";
    }
    if (!gate.passed) {
      explanation.add(gate.code, subject, gate.detail);
    }
    report.gates.push_back(std::move(gate));
  }

  // --- gate: declared ride-through autonomy --------------------------------
  {
    FailoverGate gate;
    gate.gate = "ride_through_autonomy";
    std::optional<Duration> weakest;
    for (const EntityRef& entity : live) {
      const std::optional<Duration> candidate = declared_autonomy_below(context, entity);
      if (candidate.has_value() && (!weakest.has_value() || *candidate < *weakest)) {
        weakest = candidate;
      }
    }
    if (!weakest.has_value()) {
      gate.mandatory = false;
      gate.evaluable = false;
      gate.code = ReasonCode::UnsupportedCapability;
      gate.detail = "no UPS or generator below the live members declares a ride-through time, so autonomy cannot "
                    "be decided";
    } else {
      gate.mandatory = true;
      gate.evaluable = true;
      gate.passed = *weakest >= context.policy.failover.minimum_autonomy;
      gate.code = gate.passed ? ReasonCode::Ok : ReasonCode::InsufficientAutonomy;
      gate.detail = "weakest declared ride-through is " + to_compact_string(*weakest) +
                    " against the policy minimum of " +
                    to_compact_string(context.policy.failover.minimum_autonomy) +
                    "; this is configured data, not a measured ride-through";
    }
    if (!gate.passed) {
      explanation.add(gate.code, subject, gate.detail);
    }
    report.gates.push_back(std::move(gate));
  }

  // --- gate: freshness of the live set -------------------------------------
  {
    FailoverGate gate;
    gate.gate = "live_evidence_fresh";
    gate.mandatory = false;
    gate.evaluable = !live.empty();
    gate.passed = gate.evaluable && all_live_fresh;
    gate.code = gate.passed ? ReasonCode::Ok : ReasonCode::AgingEvidence;
    gate.detail = gate.passed
                      ? "every live member carries fresh evidence"
                      : (live.empty() ? "no live member exists to assess" : "at least one live member carries aging evidence");
    if (any_live_aging) {
      explanation.add(gate.code, subject, gate.detail);
    }
    for (const EntityRef& entity : live) {
      gate.evidence.push_back(select_active_power(context, entity).reference());
    }
    report.gates.push_back(std::move(gate));
  }

  std::sort(report.gates.begin(), report.gates.end());
  for (FailoverGate& gate : report.gates) {
    std::sort(gate.evidence.begin(), gate.evidence.end());
    gate.evidence.erase(std::unique(gate.evidence.begin(), gate.evidence.end()), gate.evidence.end());
  }

  std::size_t mandatory_count = 0;
  std::size_t mandatory_failures = 0;
  std::size_t mandatory_unevaluable = 0;
  std::size_t optional_failures = 0;
  for (const FailoverGate& gate : report.gates) {
    if (gate.mandatory) {
      ++mandatory_count;
      if (!gate.evaluable) {
        ++mandatory_unevaluable;
      } else if (!gate.passed) {
        ++mandatory_failures;
      }
    } else if (gate.evaluable && !gate.passed) {
      ++optional_failures;
    }
  }

  if (mandatory_failures > 0) {
    report.readiness = FailoverReadiness::NotReady;
  } else if (mandatory_unevaluable > 0) {
    report.readiness =
        mandatory_unevaluable == mandatory_count ? FailoverReadiness::Unsupported : FailoverReadiness::Unknown;
  } else if (optional_failures > 0) {
    report.readiness = FailoverReadiness::Degraded;
  } else {
    report.readiness = FailoverReadiness::Ready;
  }

  explanation.add(ReasonCode::FailoverReady, subject,
                  "readiness " + std::string(po::to_string(report.readiness)) + " from " +
                      std::to_string(report.gates.size()) + " gate(s): " +
                      std::to_string(mandatory_count - mandatory_failures - mandatory_unevaluable) + " mandatory pass, " +
                      std::to_string(mandatory_failures) + " mandatory fail, " +
                      std::to_string(mandatory_unevaluable) + " mandatory not evaluable, " +
                      std::to_string(optional_failures) + " optional fail");

  explanation.canonicalize();
  report.explanation = explanation;
  return Outcome<FailoverReport>(std::move(report), std::move(explanation));
}

}  // namespace po
