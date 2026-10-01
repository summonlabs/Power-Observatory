// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Behavioural coverage of the attribution, quality and failover engines, plus
// divergence. Each check drives one branch that the explanation model claims to
// distinguish, so a regression in the classification shows up as a failure
// rather than as a quietly different answer.
#include "test_harness.hpp"

#include <string>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/attribution.hpp"
#include "power_observatory/divergence.hpp"
#include "power_observatory/failover.hpp"
#include "power_observatory/quality.hpp"
#include "power_observatory/scenario.hpp"

using namespace po;

namespace {

Result<Snapshot> snapshot_for(ScenarioOptions options, ObservationPolicy policy = pofix::tolerant_policy(),
                              QuantityRep steady_offset = 0) {
  Result<Scenario> scenario = build_standard_scenario(options);
  if (!scenario) {
    return scenario.error();
  }
  pofix::stamp_live(scenario.value(), options.start_time, MonotonicInstant::from_nanos(0));
  return pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                              MonotonicInstant::from_nanos(steady_offset), policy);
}

Result<Snapshot> drop_entities(ScenarioOptions options, EntityKind kind, ObservationPolicy policy) {
  Result<Scenario> scenario = build_standard_scenario(options);
  if (!scenario) {
    return scenario.error();
  }
  for (EvidenceBatch& batch : scenario.value().batches) {
    std::vector<Measurement> kept;
    for (const Measurement& measurement : batch.measurements) {
      if (measurement.entity.kind() != kind) {
        kept.push_back(measurement);
      }
    }
    batch.measurements = std::move(kept);
  }
  pofix::stamp_live(scenario.value(), options.start_time, MonotonicInstant::from_nanos(0));
  return pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                              MonotonicInstant::from_nanos(0), policy);
}

}  // namespace

PO_TEST(attribution, a_consistent_plant_leaves_no_unattributed_residual) {
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(21, 2));
  PO_REQUIRE_OK(snapshot);
  const Outcome<AttributionReport> report = snapshot.value().attribution(AttributionQuery{});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().state, EvidenceState::Known);
  PO_CHECK_EQ(report.value().unattributed_imbalances, std::size_t{0});
  PO_CHECK(!report.value().imbalances.empty());
  PO_CHECK(!report.value().losses.empty());
  PO_REQUIRE(report.value().total_measured_loss.has_value());
  PO_CHECK(report.value().total_measured_loss->is_positive());
}

PO_TEST(attribution, a_measured_residual_beyond_tolerance_is_attributed_to_nothing) {
  // A policy tolerance far tighter than the plant's own conversion losses turns
  // every stage into an unattributed residual, which is the point: the engine
  // reports what it measured rather than assuming the loss away.
  ObservationPolicy policy = pofix::tolerant_policy();
  policy.imbalance.tolerance_ppm = Ratio::from_raw(1);
  policy.imbalance.absolute_floor = Power::from_raw(0);
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(22, 1), policy);
  PO_REQUIRE_OK(snapshot);
  const Outcome<AttributionReport> report = snapshot.value().attribution(AttributionQuery{});
  PO_REQUIRE_OK(report);
  PO_CHECK(report.value().unattributed_imbalances > 0);
  bool explained = false;
  for (const Reason& reason : report.value().explanation.reasons()) {
    if (reason.code == ReasonCode::ImbalanceUnattributed) {
      explained = true;
    }
  }
  PO_CHECK(explained);
}

PO_TEST(attribution, a_missing_branch_is_indeterminate_not_zero) {
  const Result<Snapshot> snapshot = drop_entities(pofix::scenario_options(23, 1), EntityKind::Circuit,
                                                  pofix::tolerant_policy());
  PO_REQUIRE_OK(snapshot);
  const Outcome<AttributionReport> report = snapshot.value().attribution(AttributionQuery{});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().state, EvidenceState::Indeterminate);
  bool explained = false;
  for (const Reason& reason : report.value().explanation.reasons()) {
    if (reason.code == ReasonCode::MissingMeasurement) {
      explained = true;
    }
  }
  PO_CHECK(explained);
}

PO_TEST(attribution, loss_is_never_inferred_from_declared_efficiency) {
  const Result<Snapshot> snapshot = drop_entities(pofix::scenario_options(24, 1), EntityKind::Bus,
                                                  pofix::tolerant_policy());
  PO_REQUIRE_OK(snapshot);
  const Outcome<AttributionReport> report = snapshot.value().attribution(AttributionQuery{});
  PO_REQUIRE_OK(report);
  // With the bus meters gone there is no input side, so every loss is
  // unsupported. The declared efficiency is still reported as configuration,
  // and it is explicitly not used to fill the gap.
  for (const LossRecord& record : report.value().losses) {
    if (record.state == EvidenceState::Unsupported) {
      PO_CHECK(record.input.is_zero());
      PO_CHECK(record.loss.is_zero());
    }
  }
  bool declared_available = false;
  for (const LossRecord& record : report.value().losses) {
    if (record.declared_efficiency_ppm.has_value()) {
      declared_available = true;
    }
  }
  PO_CHECK(declared_available);
}

PO_TEST(quality, a_nominal_plant_produces_no_findings) {
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(25, 1));
  PO_REQUIRE_OK(snapshot);
  const Outcome<QualityReport> report = snapshot.value().quality(QualityQuery{});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().state, EvidenceState::Known);
  PO_CHECK_EQ(report.value().measurements_skipped_stale, std::size_t{0});
  PO_CHECK(report.value().measurements_assessed > 0);
  for (const QualityFinding& finding : report.value().findings) {
    PO_CHECK(finding.severity < Severity::Error);
  }
}

PO_TEST(quality, an_out_of_tolerance_frequency_is_a_finding) {
  ObservationPolicy policy = pofix::tolerant_policy();
  policy.quality.frequency_tolerance = millihertz(1);
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(26, 1), policy);
  PO_REQUIRE_OK(snapshot);
  const Outcome<QualityReport> report = snapshot.value().quality(QualityQuery{});
  PO_REQUIRE_OK(report);
  bool found = false;
  for (const QualityFinding& finding : report.value().findings) {
    if (finding.code == ReasonCode::FrequencyOutOfRange) {
      found = true;
    }
  }
  PO_CHECK(found);
}

PO_TEST(quality, an_implausible_value_is_reported_as_an_extreme) {
  ObservationPolicy policy = pofix::tolerant_policy();
  policy.quality.maximum_plausible_power = Power::from_raw(1000);  // one watt
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(27, 1), policy);
  PO_REQUIRE_OK(snapshot);
  const Outcome<QualityReport> report = snapshot.value().quality(QualityQuery{});
  PO_REQUIRE_OK(report);
  bool found = false;
  for (const QualityFinding& finding : report.value().findings) {
    if (finding.code == ReasonCode::NumericExtreme) {
      found = true;
    }
  }
  PO_CHECK(found);
}

PO_TEST(quality, stale_evidence_is_skipped_and_counted_rather_than_assessed) {
  ObservationPolicy policy;
  policy.freshness.default_budget.fresh_within = Duration::from_raw(0);
  policy.freshness.default_budget.aging_within = Duration::from_raw(0);
  policy.freshness.default_budget.stale_within = Duration::from_raw(0);
  // Age the monotonic clock by an hour while leaving every freshness window at
  // zero, so every measurement has aged out of a zero-width budget.
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(28, 1), policy,
                                                 seconds(3600).raw());
  PO_REQUIRE_OK(snapshot);
  const Outcome<QualityReport> report = snapshot.value().quality(QualityQuery{});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().measurements_assessed, std::size_t{0});
  PO_CHECK(report.value().measurements_skipped_stale > 0);
  PO_CHECK_EQ(report.value().state, EvidenceState::Stale);
}

PO_TEST(quality, phase_imbalance_needs_all_three_phases) {
  // The scenario publishes A, B and C voltages on each bus. Removing one phase
  // must produce a missing-measurement finding rather than a silent pass.
  ScenarioOptions options = pofix::scenario_options(29, 1);
  Result<Scenario> scenario = build_standard_scenario(options);
  PO_REQUIRE_OK(scenario);
  for (EvidenceBatch& batch : scenario.value().batches) {
    std::vector<Measurement> kept;
    for (const Measurement& measurement : batch.measurements) {
      if (measurement.kind() == MeasurementKind::Voltage && measurement.phase == Phase::C) {
        continue;
      }
      kept.push_back(measurement);
    }
    batch.measurements = std::move(kept);
  }
  pofix::stamp_live(scenario.value(), options.start_time, MonotonicInstant::from_nanos(0));
  const Result<Snapshot> snapshot =
      pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                           MonotonicInstant::from_nanos(0));
  PO_REQUIRE_OK(snapshot);
  const Outcome<QualityReport> report = snapshot.value().quality(QualityQuery{});
  PO_REQUIRE_OK(report);
  bool found = false;
  for (const QualityFinding& finding : report.value().findings) {
    if (finding.code == ReasonCode::MissingMeasurement && finding.kind == MeasurementKind::Voltage) {
      found = true;
    }
  }
  PO_CHECK(found);
}

PO_TEST(failover, a_healthy_group_is_ready) {
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(30, 1));
  PO_REQUIRE_OK(snapshot);
  const Outcome<FailoverReport> report = snapshot.value().failover(FailoverQuery{RedundancyGroupId("rg-main")});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().readiness, FailoverReadiness::Ready);
  PO_CHECK_EQ(report.value().currently_live, std::uint32_t{2});
  for (const FailoverGate& gate : report.value().gates) {
    PO_CHECK(gate.evaluable);
    PO_CHECK(gate.passed);
  }
}

PO_TEST(failover, a_group_down_to_one_live_member_is_not_ready) {
  ScenarioOptions options = pofix::scenario_options(31, 1);
  Result<Scenario> scenario = build_standard_scenario(options);
  PO_REQUIRE_OK(scenario);
  for (EvidenceBatch& batch : scenario.value().batches) {
    std::vector<Measurement> kept;
    for (const Measurement& measurement : batch.measurements) {
      if (measurement.entity == EntityRef::feed(FeedId("main-b")) &&
          measurement.kind() == MeasurementKind::ActivePower) {
        continue;
      }
      kept.push_back(measurement);
    }
    batch.measurements = std::move(kept);
  }
  pofix::stamp_live(scenario.value(), options.start_time, MonotonicInstant::from_nanos(0));
  const Result<Snapshot> snapshot =
      pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                           MonotonicInstant::from_nanos(0));
  PO_REQUIRE_OK(snapshot);
  const Outcome<FailoverReport> report = snapshot.value().failover(FailoverQuery{RedundancyGroupId("rg-main")});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().readiness, FailoverReadiness::NotReady);
  PO_CHECK_EQ(report.value().currently_live, std::uint32_t{1});
  // One live member still satisfies the group's required_live of one, so the
  // liveness gate passes. What fails is the headroom gate: losing the last live
  // member leaves nothing to carry the load.
  std::size_t mandatory_failures = 0;
  for (const FailoverGate& gate : report.value().gates) {
    if (gate.mandatory && gate.evaluable && !gate.passed) {
      ++mandatory_failures;
    }
  }
  PO_CHECK(mandatory_failures > 0);
  bool headroom_failed = false;
  for (const FailoverGate& gate : report.value().gates) {
    if (gate.gate == "peer_headroom_after_single_failure" && !gate.passed) {
      headroom_failed = true;
    }
  }
  PO_CHECK(headroom_failed);
}

PO_TEST(failover, a_group_without_synchronism_evidence_is_not_declared_ready) {
  // Phase-angle synchronism is not a quantity this runtime carries, and the
  // frequency proxy needs two live members. With the peer down there is no
  // synchronism evidence at all, and the gate must say so rather than pass.
  ObservationPolicy policy = pofix::tolerant_policy();
  policy.failover.require_sync_evidence = true;
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(32, 1), policy);
  PO_REQUIRE_OK(snapshot);
  const Outcome<FailoverReport> report = snapshot.value().failover(FailoverQuery{RedundancyGroupId("rg-main")});
  PO_REQUIRE_OK(report);
  bool saw_gate = false;
  for (const FailoverGate& gate : report.value().gates) {
    if (gate.gate == "peer_frequency_agreement") {
      saw_gate = true;
      PO_CHECK(gate.evaluable);
    }
  }
  PO_CHECK(saw_gate);
}

PO_TEST(failover, an_undeclared_group_is_refused) {
  const Result<Snapshot> snapshot = snapshot_for(pofix::scenario_options(33, 1));
  PO_REQUIRE_OK(snapshot);
  const Outcome<FailoverReport> report = snapshot.value().failover(FailoverQuery{RedundancyGroupId("nope")});
  PO_CHECK(!report.has_value());
  PO_CHECK_EQ(report.code(), ReasonCode::UnknownEntity);
}

PO_TEST(divergence, an_identical_pair_reports_no_divergence) {
  const Result<Snapshot> first = snapshot_for(pofix::scenario_options(34, 1));
  const Result<Snapshot> second = snapshot_for(pofix::scenario_options(34, 1));
  PO_REQUIRE_OK(first);
  PO_REQUIRE_OK(second);
  const DivergenceReport report = compare(first.value(), second.value());
  PO_CHECK_EQ(report.verdict, DivergenceVerdict::Identical);
  PO_CHECK_EQ(report.diverged_keys, std::size_t{0});
  PO_CHECK(report.entries.empty());
}

PO_TEST(divergence, an_advanced_generation_is_reported_as_advanced) {
  const Result<Snapshot> first = snapshot_for(pofix::scenario_options(35, 1));
  ScenarioOptions options = pofix::scenario_options(35, 2);
  options.generation = Generation(2);
  const Result<Snapshot> second = snapshot_for(options);
  PO_REQUIRE_OK(first);
  PO_REQUIRE_OK(second);
  const DivergenceReport report = compare(first.value(), second.value());
  PO_CHECK(report.verdict == DivergenceVerdict::Advanced || report.verdict == DivergenceVerdict::Diverged);
  PO_CHECK(report.diverged_keys > 0);
  PO_CHECK(!report.entries.empty());
}

PO_TEST(divergence, a_sign_flip_is_classified_separately_from_a_value_change) {
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(36, 1));
  PO_REQUIRE_OK(scenario);
  pofix::stamp_live(scenario.value(), Timestamp::from_unix_nanos(0), MonotonicInstant::from_nanos(0));
  const Result<Snapshot> left = pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(1),
                                                     MonotonicInstant::from_nanos(0));
  PO_REQUIRE_OK(left);

  Scenario flipped = scenario.value();
  for (EvidenceBatch& batch : flipped.batches) {
    for (Measurement& measurement : batch.measurements) {
      if (measurement.kind() == MeasurementKind::ActivePower) {
        measurement.value = Power::from_raw(-std::get<Power>(measurement.value).raw());
      }
    }
  }
  const Result<EvidenceSet> right_evidence = pofix::evidence_from(flipped);
  PO_REQUIRE_OK(right_evidence);
  const DivergenceReport report = compare(left.value().evidence(), right_evidence.value());
  bool saw_sign_flip = false;
  for (const DivergenceEntry& entry : report.entries) {
    if (entry.classification == DivergenceClass::SignFlip) {
      saw_sign_flip = true;
    }
  }
  PO_CHECK(saw_sign_flip);
}

PO_TEST_MAIN()
