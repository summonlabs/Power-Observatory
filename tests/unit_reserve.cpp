// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <functional>
#include <string>

#include "po_fixtures.hpp"
#include "power_observatory/reserve.hpp"
#include "power_observatory/scenario.hpp"

using namespace po;

namespace {

struct Fixture {
  Scenario scenario;
  Snapshot snapshot;
};

Result<Snapshot> build(std::uint64_t seed, std::size_t steps = 1,
                       const std::function<void(ScenarioOptions&)>& tweak = nullptr) {
  ScenarioOptions options = pofix::scenario_options(seed, steps);
  if (tweak) {
    tweak(options);
  }
  Result<Scenario> scenario = build_standard_scenario(options);
  if (!scenario) {
    return scenario.error();
  }
  pofix::stamp_live(scenario.value(), options.start_time, MonotonicInstant::from_nanos(0));
  return pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                              MonotonicInstant::from_nanos(0));
}

}  // namespace

PO_TEST(reserve, nominal_scenario_reports_capacity_load_and_headroom) {
  const Result<Snapshot> snapshot = build(11);
  PO_REQUIRE_OK(snapshot);
  const Outcome<ReserveReport> report = snapshot.value().reserve(
      ReserveQuery{EntityRef::redundancy_group(RedundancyGroupId("rg-main")), true});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().state, EvidenceState::Known);
  PO_REQUIRE(report.value().usable_capacity.has_value());
  PO_REQUIRE(report.value().measured_load.has_value());
  PO_REQUIRE(report.value().reserve.has_value());
  PO_CHECK_EQ(report.value().usable_capacity->raw(), QuantityRep{2000000000});
  PO_CHECK(!report.value().measured_load->is_zero());
  PO_CHECK_EQ(report.value().reserve->raw(),
              report.value().usable_capacity->raw() - report.value().measured_load->raw());
  PO_CHECK(report.value().known_components == report.value().total_components);
  PO_CHECK(!report.value().below_minimum_headroom);
}

PO_TEST(reserve, single_failure_headroom_is_computed_by_its_own_model) {
  const Result<Snapshot> snapshot = build(12);
  PO_REQUIRE_OK(snapshot);
  const Outcome<ReserveReport> report = snapshot.value().reserve(
      ReserveQuery{EntityRef::redundancy_group(RedundancyGroupId("rg-main")), true});
  PO_REQUIRE_OK(report);
  PO_REQUIRE(report.value().single_failure_reserve.has_value());
  // Losing the largest contributor removes exactly one feed's capacity, so the
  // single-failure headroom is the plain reserve minus that capacity. The two
  // are computed independently and must agree.
  PO_CHECK_EQ(report.value().single_failure_reserve->raw(),
              report.value().reserve->raw() - QuantityRep{1000000000});
  PO_CHECK(report.value().single_failure_capable);
}

PO_TEST(reserve, a_derated_feed_still_publishes_the_derated_capacity) {
  const Result<Snapshot> snapshot = build(13, 1, [](ScenarioOptions& options) {
    options.derate_feed_ppm = 500000;  // halve what feed main-a delivers
  });
  PO_REQUIRE_OK(snapshot);
  const Outcome<ReserveReport> report = snapshot.value().reserve(
      ReserveQuery{EntityRef::redundancy_group(RedundancyGroupId("rg-main")), true});
  PO_REQUIRE_OK(report);
  // The derate is applied to the measured delivery of the synthetic plant, not
  // to the declared capacity, so capacity stays at the declared total and one
  // member simply carries less.
  PO_CHECK_EQ(report.value().usable_capacity->raw(), QuantityRep{2000000000});
  PO_REQUIRE(report.value().measured_load.has_value());
  std::optional<QuantityRep> derated;
  std::optional<QuantityRep> untouched;
  for (const ReserveComponent& component : report.value().components) {
    if (component.entity == EntityRef::feed(FeedId("main-a"))) {
      derated = component.measured_load.has_value() ? std::optional<QuantityRep>(component.measured_load->raw())
                                                    : std::nullopt;
    }
    if (component.entity == EntityRef::feed(FeedId("main-b"))) {
      untouched = component.measured_load.has_value() ? std::optional<QuantityRep>(component.measured_load->raw())
                                                      : std::nullopt;
    }
  }
  PO_REQUIRE(derated.has_value());
  PO_REQUIRE(untouched.has_value());
  // The derate halves what the feed delivers, so the affected member clearly
  // carries less than its untouched peer while still carrying real load.
  PO_CHECK(*derated < *untouched);
  PO_CHECK(*derated < (*untouched * 3) / 4);
  PO_CHECK(*derated > *untouched / 4);
}

PO_TEST(reserve, policy_derate_scales_declared_capacity) {
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(14, 1));
  PO_REQUIRE_OK(scenario);
  pofix::stamp_live(scenario.value(), pofix::scenario_options(14, 1).start_time,
                    MonotonicInstant::from_nanos(0));
  ObservationPolicy policy = pofix::tolerant_policy();
  policy.reserve.derate_ppm = Ratio::from_raw(800000);
  const Result<Snapshot> snapshot =
      pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(1), MonotonicInstant::from_nanos(0), policy);
  PO_REQUIRE_OK(snapshot);
  const Outcome<ReserveReport> report = snapshot.value().reserve(
      ReserveQuery{EntityRef::redundancy_group(RedundancyGroupId("rg-main")), true});
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().usable_capacity->raw(), QuantityRep{1600000000});
  bool derated_reason = false;
  for (const Reason& reason : report.value().explanation.reasons()) {
    if (reason.code == ReasonCode::CapacityDerated) {
      derated_reason = true;
    }
  }
  PO_CHECK(derated_reason);
}

PO_TEST(reserve, out_of_service_members_contribute_no_capacity) {
  // A group whose member declarations are not InService cannot supply reserve,
  // however healthy any measurement looks.
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(15, 1));
  PO_REQUIRE_OK(scenario);
  TopologyModel topology = scenario.value().topology;
  FeedSet feeds = topology.feeds();
  for (Feed& feed : feeds) {
    feed.declared_state = LifecycleState::Maintenance;
  }
  const Result<TopologyModel> rebuilt = TopologyModel::build(
      std::move(feeds), topology.buses(), topology.ups_units(), topology.generators(), topology.pdus(),
      topology.circuits(), topology.loads(), topology.redundancy_groups());
  PO_REQUIRE_OK(rebuilt);

  Scenario degraded = scenario.value();
  degraded.topology = std::move(rebuilt).value();
  pofix::stamp_live(degraded, Timestamp::from_unix_nanos(0), MonotonicInstant::from_nanos(0));
  const Result<Snapshot> snapshot =
      pofix::snapshot_from(degraded, Timestamp::from_unix_nanos(1), MonotonicInstant::from_nanos(0));
  PO_REQUIRE_OK(snapshot);
  const Outcome<ReserveReport> report = snapshot.value().reserve(
      ReserveQuery{EntityRef::redundancy_group(RedundancyGroupId("rg-main")), true});
  PO_REQUIRE_OK(report);
  PO_REQUIRE(report.value().usable_capacity.has_value());
  PO_CHECK_EQ(report.value().usable_capacity->raw(), QuantityRep{0});
  PO_CHECK(!report.value().single_failure_capable);
}

PO_TEST(reserve, an_undeclared_scope_is_refused) {
  const Result<Snapshot> snapshot = build(16);
  PO_REQUIRE_OK(snapshot);
  const Outcome<ReserveReport> report =
      snapshot.value().reserve(ReserveQuery{EntityRef::feed(FeedId("nope")), true});
  PO_CHECK(!report.has_value());
  PO_CHECK_EQ(report.code(), ReasonCode::UnknownEntity);
  PO_CHECK(!report.explanation().reasons().empty());
}

PO_TEST(reserve, unknown_load_suppresses_the_total_rather_than_reporting_zero) {
  ScenarioOptions options = pofix::scenario_options(17, 1);
  Result<Scenario> scenario = build_standard_scenario(options);
  PO_REQUIRE_OK(scenario);
  // Drop every feed measurement so the supply-side load is unknown.
  Scenario stripped = scenario.value();
  for (EvidenceBatch& batch : stripped.batches) {
    std::vector<Measurement> kept;
    for (const Measurement& measurement : batch.measurements) {
      if (measurement.entity.kind() != EntityKind::Feed) {
        kept.push_back(measurement);
      }
    }
    batch.measurements = std::move(kept);
  }
  pofix::stamp_live(stripped, options.start_time, MonotonicInstant::from_nanos(0));
  const Result<Snapshot> snapshot =
      pofix::snapshot_from(stripped, Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                           MonotonicInstant::from_nanos(0));
  PO_REQUIRE_OK(snapshot);
  const Outcome<ReserveReport> report = snapshot.value().reserve(
      ReserveQuery{EntityRef::redundancy_group(RedundancyGroupId("rg-main")), true});
  PO_REQUIRE_OK(report);
  PO_CHECK(!report.value().reserve.has_value());
  PO_CHECK_EQ(report.value().state, EvidenceState::Unknown);
  PO_REQUIRE(report.value().usable_capacity.has_value());
  bool explained = false;
  for (const Reason& reason : report.value().explanation.reasons()) {
    if (reason.code == ReasonCode::ReserveUnknown) {
      explained = true;
    }
  }
  PO_CHECK(explained);
}

PO_TEST_MAIN()
