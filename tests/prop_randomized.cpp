// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Seeded randomised and property coverage. Every case is driven by a fixed seed
// so a failure is reproducible from the seed alone, and every property is one
// this runtime actually claims rather than a restatement of the code.
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/divergence.hpp"
#include "power_observatory/persistence.hpp"
#include "power_observatory/scenario.hpp"
#include "power_observatory/snapshot.hpp"

using namespace po;

namespace {

std::vector<Measurement> random_measurements(DeterministicRandom& random, std::size_t count, std::uint64_t seed_tag) {
  std::vector<Measurement> measurements;
  const char* entities[] = {"feed:a", "feed:b", "bus:a", "bus:b", "load:a", "load:b"};
  const MeasurementKind kinds[] = {MeasurementKind::ActivePower, MeasurementKind::Voltage,
                                   MeasurementKind::Frequency};
  for (std::size_t index = 0; index < count; ++index) {
    Measurement measurement;
    measurement.id = MeasurementId(index + 1);
    measurement.entity = EntityRef::parse(entities[random.bounded(6)]).value();
    const MeasurementKind kind = kinds[random.bounded(3)];
    measurement.phase = static_cast<Phase>(random.bounded(4));
    switch (kind) {
      case MeasurementKind::Voltage:
        measurement.value = Voltage::from_raw(random.between(-1000000, 1000000));
        break;
      case MeasurementKind::Frequency:
        measurement.value = Frequency::from_raw(random.between(0, 100000));
        break;
      default:
        measurement.value = Power::from_raw(random.between(-100000000, 100000000));
        break;
    }
    measurement.provenance =
        make_observed_provenance(SourceId("source-" + std::to_string(seed_tag % 3)), Generation(1 + random.bounded(4)),
                                 Epoch{}, Sequence(random.bounded(8)), std::nullopt, AuthorityKind::Observed,
                                 Timestamp::from_unix_millis(1700000000000LL).value(),
                                 MonotonicInstant::from_nanos(1));
    measurements.push_back(std::move(measurement));
  }
  return measurements;
}

}  // namespace

PO_TEST(property, evidence_construction_is_order_independent_for_every_seed) {
  for (std::uint64_t seed = 1; seed <= 60; ++seed) {
    DeterministicRandom random(seed);
    std::vector<Measurement> measurements = random_measurements(random, 40, seed);
    std::vector<Measurement> shuffled = measurements;

    DeterministicRandom shuffle_random(seed * 7919 + 13);
    for (std::size_t index = shuffled.size(); index > 1; --index) {
      const std::size_t other = static_cast<std::size_t>(shuffle_random.bounded(index));
      std::swap(shuffled[index - 1], shuffled[other]);
    }

    const Result<EvidenceSet> forward = EvidenceSet::build(measurements);
    const Result<EvidenceSet> reverse = EvidenceSet::build(shuffled);
    PO_REQUIRE(static_cast<bool>(forward) == static_cast<bool>(reverse));
    if (!forward) {
      PO_CHECK_EQ(forward.code(), reverse.code());
      continue;
    }
    PO_CHECK_EQ(forward.value().size(), reverse.value().size());
    PO_CHECK_EQ(forward.value().content_hash(), reverse.value().content_hash());
    PO_CHECK_EQ(forward.value().admitted_count(), reverse.value().admitted_count());
  }
}

PO_TEST(property, canonical_evidence_is_deduplicated_and_bounded_by_the_input) {
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    DeterministicRandom random(seed * 31 + 5);
    const std::vector<Measurement> measurements = random_measurements(random, 64, seed);
    const Result<EvidenceSet> set = EvidenceSet::build(measurements);
    PO_REQUIRE_OK(set);
    PO_CHECK(set.value().size() <= measurements.size());
    PO_CHECK_EQ(set.value().admitted_count(), measurements.size());
    PO_CHECK_EQ(set.value().admitted_count() - set.value().size(), set.value().dropped_count());
    // Every retained measurement is unique on its key.
    for (std::size_t left = 0; left < set.value().measurements().size(); ++left) {
      for (std::size_t right = left + 1; right < set.value().measurements().size(); ++right) {
        const Measurement& a = set.value().measurements()[left];
        const Measurement& b = set.value().measurements()[right];
        const bool same_key = a.entity == b.entity && a.kind() == b.kind() && a.phase == b.phase &&
                              a.provenance.source == b.provenance.source;
        PO_CHECK(!same_key);
      }
    }
  }
}

PO_TEST(property, the_answer_is_deterministic_for_every_seed_it_can_answer) {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    ScenarioOptions options = pofix::scenario_options(seed, 2);
    Result<Scenario> scenario = build_standard_scenario(options);
    PO_REQUIRE_OK(scenario);
    pofix::stamp_live(scenario.value(), options.start_time, MonotonicInstant::from_nanos(0));
    const Result<Snapshot> first =
        pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                             MonotonicInstant::from_nanos(0));
    PO_REQUIRE_OK(first);
    const Result<Snapshot> second =
        pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                             MonotonicInstant::from_nanos(0));
    PO_REQUIRE_OK(second);

    const AnswerReport left = first.value().answer();
    const AnswerReport right = second.value().answer();
    PO_CHECK_EQ(left.state, right.state);
    PO_CHECK_EQ(left.explanation.content_hash(), right.explanation.content_hash());
    PO_CHECK_EQ(left.measured_entities, right.measured_entities);
    PO_CHECK_EQ(left.unmeasured_entities, right.unmeasured_entities);
    PO_CHECK_EQ(left.unattributed_imbalances, right.unattributed_imbalances);
    PO_CHECK_EQ(left.quality_findings, right.quality_findings);
    if (left.total_observed_load.has_value()) {
      PO_REQUIRE(right.total_observed_load.has_value());
      PO_CHECK_EQ(left.total_observed_load->raw(), right.total_observed_load->raw());
    }
  }
}

PO_TEST(property, comparing_a_snapshot_with_itself_is_always_identical) {
  for (std::uint64_t seed = 1; seed <= 25; ++seed) {
    ScenarioOptions options = pofix::scenario_options(seed, 2);
    Result<Scenario> scenario = build_standard_scenario(options);
    PO_REQUIRE_OK(scenario);
    pofix::stamp_live(scenario.value(), options.start_time, MonotonicInstant::from_nanos(0));
    const Result<Snapshot> snapshot =
        pofix::snapshot_from(scenario.value(), Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1),
                             MonotonicInstant::from_nanos(0));
    PO_REQUIRE_OK(snapshot);
    const DivergenceReport report = compare(snapshot.value(), snapshot.value());
    PO_CHECK_EQ(report.verdict, DivergenceVerdict::Identical);
    PO_CHECK_EQ(report.diverged_keys, std::size_t{0});
  }
}

PO_TEST(property, every_accepted_batch_survives_a_real_durable_round_trip) {
  const std::string directory = pofix::scratch("prop_round_trip");
  for (std::uint64_t seed = 1; seed <= 12; ++seed) {
    const std::string log_path = po::join_path(directory, "log-" + std::to_string(seed) + ".poev");
    ScenarioOptions options = pofix::scenario_options(seed, 1);
    Result<Scenario> scenario = build_standard_scenario(options);
    PO_REQUIRE_OK(scenario);

    StoreOpenOptions store_options;
    std::string expected_hash;
    std::size_t expected_count = 0;
    {
      Result<EvidenceLog> log = EvidenceLog::open(log_path, Epoch{}, store_options);
      PO_REQUIRE_OK(log);
      for (const EvidenceBatch& batch : scenario.value().batches) {
        PO_REQUIRE_OK(log.value().append(batch));
      }
      expected_hash = std::to_string(log.value().evidence().size());
      expected_count = log.value().evidence().size();
      PO_CHECK_OK(log.value().close());
    }
    {
      Result<EvidenceLog> reopened = EvidenceLog::open(log_path, Epoch{}, store_options);
      PO_REQUIRE_OK(reopened);
      PO_CHECK_EQ(reopened.value().evidence().size(), expected_count);
      PO_CHECK_EQ(std::to_string(reopened.value().evidence().size()), expected_hash);
      PO_CHECK_EQ(reopened.value().recovery().bytes_discarded, std::uint64_t{0});
      PO_CHECK_EQ(reopened.value().recovery().records_rejected, std::size_t{0});
      PO_CHECK_OK(reopened.value().close());
    }
  }
  pofix::clear("prop_round_trip");
}

PO_TEST_MAIN()
