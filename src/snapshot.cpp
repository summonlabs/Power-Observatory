// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/snapshot.hpp"

#include <algorithm>
#include <string>

#include "power_observatory/hashing.hpp"

namespace po {
namespace {

[[nodiscard]] std::uint64_t hash_policy(const ObservationPolicy& policy) noexcept {
  Fnv1a64 hasher;
  hasher.update_separator('f');
  hasher.update_integral(policy.freshness.default_budget.fresh_within.raw());
  hasher.update_integral(policy.freshness.default_budget.aging_within.raw());
  hasher.update_integral(policy.freshness.default_budget.stale_within.raw());
  hasher.update_integral(policy.freshness.max_clock_skew.raw());
  for (const auto& entry : policy.freshness.per_source) {
    hasher.update(entry.first.view());
    hasher.update_integral(entry.second.fresh_within.raw());
    hasher.update_integral(entry.second.aging_within.raw());
    hasher.update_integral(entry.second.stale_within.raw());
  }
  hasher.update_separator('i');
  hasher.update_integral(policy.imbalance.tolerance_ppm.raw());
  hasher.update_integral(policy.imbalance.absolute_floor.raw());
  hasher.update_separator('l');
  hasher.update_integral(policy.loss.max_loss_ppm.raw());
  hasher.update_integral(policy.loss.absolute_floor.raw());
  hasher.update_separator('q');
  hasher.update_integral(policy.quality.nominal_frequency.raw());
  hasher.update_integral(policy.quality.frequency_tolerance.raw());
  hasher.update_integral(policy.quality.nominal_voltage.raw());
  hasher.update_integral(policy.quality.voltage_tolerance_ppm.raw());
  hasher.update_integral(policy.quality.minimum_power_factor_ppm.raw());
  hasher.update_integral(policy.quality.phase_imbalance_limit_ppm.raw());
  hasher.update_integral(policy.quality.maximum_plausible_power.raw());
  hasher.update_integral(policy.quality.maximum_plausible_current.raw());
  hasher.update_separator('r');
  hasher.update_integral(policy.reserve.derate_ppm.raw());
  hasher.update_integral(policy.reserve.minimum_headroom.raw());
  hasher.update_integral(policy.reserve.require_declared_redundancy ? 1u : 0u);
  hasher.update_separator('v');
  hasher.update_integral(policy.failover.max_transfer_time.raw());
  hasher.update_integral(policy.failover.minimum_autonomy.raw());
  hasher.update_integral(policy.failover.minimum_peer_margin_ppm.raw());
  hasher.update_integral(policy.failover.require_peer_live ? 1u : 0u);
  hasher.update_integral(policy.failover.require_sync_evidence ? 1u : 0u);
  return hasher.value();
}

[[nodiscard]] std::string optional_power_text(const std::optional<Power>& value) {
  return value.has_value() ? value->to_string() : std::string("unknown");
}

}  // namespace

Result<Snapshot> Snapshot::build(Revision revision, EvidenceSet evidence, TopologyModel topology,
                                 ObservationPolicy policy, Timestamp computed_at,
                                 MonotonicInstant computed_steady) {
  const Status valid = policy.validate();
  if (!valid) {
    return valid.error();
  }

  Snapshot snapshot;
  snapshot.metadata_.revision = revision;
  snapshot.metadata_.epoch = Epoch{};
  snapshot.metadata_.computed_at = computed_at;
  snapshot.metadata_.computed_steady = computed_steady;
  snapshot.metadata_.measurement_count = evidence.size();
  snapshot.metadata_.dropped_measurement_count = evidence.dropped_count();
  snapshot.metadata_.generations = evidence.generations();
  snapshot.metadata_.evidence_hash = evidence.content_hash();
  snapshot.metadata_.topology_hash = topology.content_hash();
  snapshot.metadata_.policy_hash = hash_policy(policy);

  snapshot.evidence_ = std::move(evidence);
  snapshot.topology_ = std::move(topology);
  snapshot.policy_ = std::move(policy);
  snapshot.freshness_ = FreshnessModel(snapshot.policy_.freshness);
  return snapshot;
}

Outcome<FlowReport> Snapshot::flow(const FlowQuery& query) const { return compute_flow(context(), query); }

Outcome<ReserveReport> Snapshot::reserve(const ReserveQuery& query) const {
  return compute_reserve(context(), query);
}

Outcome<AttributionReport> Snapshot::attribution(const AttributionQuery& query) const {
  return compute_attribution(context(), query);
}

Outcome<QualityReport> Snapshot::quality(const QualityQuery& query) const {
  return compute_quality(context(), query);
}

Outcome<FailoverReport> Snapshot::failover(const FailoverQuery& query) const {
  return compute_failover(context(), query);
}

AnswerReport Snapshot::answer() const {
  AnswerReport report;
  report.revision = metadata_.revision;
  report.computed_at = metadata_.computed_at;

  Explanation explanation;

  const Outcome<FlowReport> flow_report = flow(FlowQuery{});
  if (flow_report.has_value()) {
    report.total_observed_load = flow_report.value().total_observed_load;
    report.unmeasured_entities = flow_report.value().unmeasured_nodes;
    report.measured_entities = flow_report.value().measured_nodes;
    explanation.merge(flow_report.value().explanation);
  } else {
    explanation.add(flow_report.code(), std::string("<flow>"), flow_report.detail());
  }

  const Outcome<AttributionReport> attribution_report = attribution(AttributionQuery{});
  if (attribution_report.has_value()) {
    report.unattributed_imbalances = attribution_report.value().unattributed_imbalances;
    explanation.merge(attribution_report.value().explanation);
  } else {
    explanation.add(attribution_report.code(), std::string("<attribution>"), attribution_report.detail());
  }

  const Outcome<QualityReport> quality_report = quality(QualityQuery{});
  if (quality_report.has_value()) {
    report.quality_findings = quality_report.value().findings.size();
    explanation.merge(quality_report.value().explanation);
  } else {
    explanation.add(quality_report.code(), std::string("<quality>"), quality_report.detail());
  }

  Accumulator<PowerUnitTag> capacity_total;
  Accumulator<PowerUnitTag> reserve_total;
  bool capacity_complete = true;
  bool reserve_complete = true;
  std::size_t group_index = 0;

  for (const RedundancyGroup& group : topology_.redundancy_groups()) {
    const EntityRef scope = EntityRef::redundancy_group(group.id);
    const Outcome<ReserveReport> reserve_report = reserve(ReserveQuery{scope, true});
    std::optional<Power> group_capacity;
    std::optional<Power> group_reserve;
    if (reserve_report.has_value()) {
      group_capacity = reserve_report.value().usable_capacity;
      group_reserve = reserve_report.value().reserve;
      explanation.merge(reserve_report.value().explanation);
    } else {
      explanation.add(reserve_report.code(), scope.to_string(), reserve_report.detail());
    }

    const Outcome<FailoverReport> failover_report = failover(FailoverQuery{group.id});
    if (failover_report.has_value()) {
      const FailoverReport& value = failover_report.value();
      explanation.merge(value.explanation);
      switch (value.readiness) {
        case FailoverReadiness::Ready:
          ++report.failover_ready;
          break;
        case FailoverReadiness::Degraded:
          ++report.failover_degraded;
          break;
        case FailoverReadiness::NotReady:
          ++report.failover_not_ready;
          break;
        case FailoverReadiness::Unknown:
        case FailoverReadiness::Unsupported:
          ++report.failover_unknown;
          break;
      }
      report.groups.push_back(value);
    } else {
      ++report.failover_unknown;
      explanation.add(failover_report.code(), scope.to_string(), failover_report.detail());
    }

    if (group_capacity.has_value()) {
      capacity_total.add(*group_capacity);
    } else {
      capacity_complete = false;
    }
    if (group_reserve.has_value()) {
      reserve_total.add(*group_reserve);
    } else {
      reserve_complete = false;
    }
    ++group_index;
  }

  if (group_index > 0 && capacity_complete && !capacity_total.overflowed()) {
    report.total_usable_capacity = capacity_total.saturated_total();
  }
  if (group_index > 0 && reserve_complete && !reserve_total.overflowed()) {
    report.total_reserve = reserve_total.saturated_total();
  }

  EvidenceState state = EvidenceState::Known;
  if (metadata_.measurement_count == 0) {
    state = EvidenceState::Unknown;
    explanation.add(ReasonCode::NoEvidence, std::string("<answer>"),
                    "the evidence set is empty, so nothing about the plant could be established");
  } else {
    // The answer can never be more current than the evidence behind it. An
    // answer assembled from expired evidence is an expired answer, however
    // internally consistent it is.
    const FreshnessAssessment evidence_freshness =
        freshness_.assess(evidence_, metadata_.computed_at, metadata_.computed_steady);
    state = worse(state, to_evidence_state(evidence_freshness.classification));
    if (evidence_freshness.classification == FreshnessClass::Expired) {
      explanation.add(ReasonCode::ExpiredEvidence, std::string("<answer>"),
                      "the newest evidence available to this answer has expired, so the answer describes a past "
                      "state rather than the current one");
    } else if (evidence_freshness.classification == FreshnessClass::Recovered) {
      explanation.add(ReasonCode::RecoveredNotFresh, std::string("<answer>"),
                      "the answer is assembled from evidence read back from durable storage; it is the last known "
                      "state rather than the current state of the plant");
    }
    if (report.unmeasured_entities > 0) {
      state = worse(state, EvidenceState::Indeterminate);
    }
    if (report.unattributed_imbalances > 0) {
      state = worse(state, EvidenceState::Conflicting);
    }
    if (report.failover_not_ready > 0) {
      state = worse(state, EvidenceState::Known);
      explanation.add(ReasonCode::FailoverBlocked, std::string("<answer>"),
                      std::to_string(report.failover_not_ready) + " redundancy group(s) are not ready to fail over");
    }
    if (report.failover_unknown > 0) {
      state = worse(state, EvidenceState::Indeterminate);
    }
  }
  report.state = state;

  explanation.add(ReasonCode::ObservedEvidence, std::string("<answer>"),
                  "answer assembled from " + std::to_string(metadata_.measurement_count) +
                      " admitted measurement(s) across " + std::to_string(metadata_.generations.size()) +
                      " source generation(s); revision " + std::to_string(metadata_.revision.value()));
  explanation.canonicalize();
  report.explanation = std::move(explanation);
  return report;
}

std::string AnswerReport::to_text() const {
  std::string text;
  text.reserve(512);
  text.append("state: ");
  text.append(po::to_string(state));
  text.append("\nrevision: ");
  text.append(std::to_string(revision.value()));
  text.append("\nevaluated as of: ");
  text.append(computed_at.to_iso8601());
  text.append("\ntotal observed load: ");
  text.append(optional_power_text(total_observed_load));
  text.append("\ntotal usable capacity: ");
  text.append(optional_power_text(total_usable_capacity));
  text.append("\ntotal reserve: ");
  text.append(optional_power_text(total_reserve));
  text.append("\nmeasured entities: ");
  text.append(std::to_string(measured_entities));
  text.append("\nunmeasured entities: ");
  text.append(std::to_string(unmeasured_entities));
  text.append("\nunattributed imbalances: ");
  text.append(std::to_string(unattributed_imbalances));
  text.append("\nquality findings: ");
  text.append(std::to_string(quality_findings));
  text.append("\nfailover groups ready/degraded/not ready/unknown: ");
  text.append(std::to_string(failover_ready));
  text.append("/");
  text.append(std::to_string(failover_degraded));
  text.append("/");
  text.append(std::to_string(failover_not_ready));
  text.append("/");
  text.append(std::to_string(failover_unknown));
  text.append("\nreasons:\n");
  text.append(explanation.to_text());
  return text;
}

}  // namespace po
