// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "power_observatory/evidence.hpp"
#include "power_observatory/hashing.hpp"

using namespace po;

namespace {

Measurement make_measurement(std::uint64_t id, std::string entity, QuantityRep watts, std::uint64_t sequence,
                             std::uint64_t generation = 1, std::string source = "meter-1",
                             Timestamp received = Timestamp::from_unix_seconds(1000).value()) {
  Measurement measurement;
  measurement.id = MeasurementId(id);
  measurement.entity = EntityRef::parse(entity).value();
  measurement.phase = Phase::Total;
  measurement.value = Power::from_raw(watts);
  measurement.provenance = make_observed_provenance(SourceId(source), Generation(generation), Epoch{},
                                                    Sequence(sequence), std::nullopt, AuthorityKind::Observed,
                                                    received, MonotonicInstant::from_nanos(1000));
  return measurement;
}

}  // namespace

PO_TEST(evidence, entity_reference_round_trips) {
  const Result<EntityRef> parsed = EntityRef::parse("redundancy_group:rg-main");
  PO_REQUIRE_OK(parsed);
  PO_CHECK_EQ(parsed.value().kind(), EntityKind::RedundancyGroup);
  PO_CHECK_EQ(parsed.value().id(), std::string("rg-main"));
  PO_CHECK_EQ(parsed.value().to_string(), std::string("redundancy_group:rg-main"));
}

PO_TEST(evidence, entity_reference_parse_is_strict) {
  PO_REQUIRE_ERR(EntityRef::parse("feed"), ReasonCode::ParseError);
  PO_REQUIRE_ERR(EntityRef::parse("feed:"), ReasonCode::ParseError);
  PO_REQUIRE_ERR(EntityRef::parse("widget:a"), ReasonCode::ParseError);
}

PO_TEST(evidence, default_entity_reference_is_invalid_and_printable) {
  const EntityRef empty;
  PO_CHECK(!empty.valid());
  PO_CHECK_EQ(empty.to_string(), std::string("unknown:"));
}

PO_TEST(evidence, duplicate_slot_is_dropped_and_noted) {
  std::vector<Measurement> input;
  input.push_back(make_measurement(1, "feed:a", 100, 5));
  input.push_back(make_measurement(2, "feed:a", 999, 5));
  const Result<EvidenceSet> set = EvidenceSet::build(std::move(input));
  PO_REQUIRE_OK(set);
  PO_CHECK_EQ(set.value().size(), std::size_t{1});
  PO_CHECK_EQ(set.value().admitted_count(), std::size_t{2});
  PO_CHECK_EQ(set.value().dropped_count(), std::size_t{1});
  bool noted = false;
  for (const EvidenceNote& note : set.value().notes()) {
    if (note.code == ReasonCode::DuplicateSample) {
      noted = true;
    }
  }
  PO_CHECK(noted);
}

PO_TEST(evidence, two_slots_in_one_batch_are_not_duplicates) {
  // A batch shares one sequence across every measurement it carries. Treating
  // that as duplication would discard the entire batch.
  std::vector<Measurement> input;
  input.push_back(make_measurement(1, "feed:a", 100, 5));
  input.push_back(make_measurement(2, "feed:b", 200, 5));
  input.push_back(make_measurement(3, "bus:a", 300, 5));
  const Result<EvidenceSet> set = EvidenceSet::build(std::move(input));
  PO_REQUIRE_OK(set);
  PO_CHECK_EQ(set.value().size(), std::size_t{3});
  PO_CHECK_EQ(set.value().dropped_count(), std::size_t{0});
}

PO_TEST(evidence, newer_generation_supersedes_and_the_loser_is_noted) {
  std::vector<Measurement> input;
  input.push_back(make_measurement(1, "feed:a", 100, 5, 1));
  input.push_back(make_measurement(2, "feed:a", 200, 6, 2));
  const Result<EvidenceSet> set = EvidenceSet::build(std::move(input));
  PO_REQUIRE_OK(set);
  PO_CHECK_EQ(set.value().size(), std::size_t{1});
  PO_CHECK_EQ(set.value().generation_of(SourceId("meter-1")).value(), std::uint64_t{2});
  bool noted = false;
  for (const EvidenceNote& note : set.value().notes()) {
    if (note.code == ReasonCode::GenerationMismatch) {
      noted = true;
    }
  }
  PO_CHECK(noted);
  const Measurement* latest = set.value().latest(EntityRef::feed(FeedId("a")), MeasurementKind::ActivePower,
                                                 Phase::Total);
  PO_REQUIRE(latest != nullptr);
  PO_CHECK_EQ(std::get<Power>(latest->value).raw(), QuantityRep{200});
}

PO_TEST(evidence, reordered_samples_are_detected_and_noted) {
  std::vector<Measurement> input;
  input.push_back(make_measurement(1, "feed:a", 100, 9));
  input.push_back(make_measurement(2, "feed:a", 200, 4));
  const Result<EvidenceSet> set = EvidenceSet::build(std::move(input));
  PO_REQUIRE_OK(set);
  PO_CHECK_EQ(set.value().size(), std::size_t{1});
  bool noted = false;
  for (const EvidenceNote& note : set.value().notes()) {
    if (note.code == ReasonCode::ReorderedSample) {
      noted = true;
    }
  }
  PO_CHECK(noted);
}

PO_TEST(evidence, arrival_order_does_not_change_the_result) {
  std::vector<Measurement> forward;
  forward.push_back(make_measurement(1, "feed:a", 100, 1));
  forward.push_back(make_measurement(2, "feed:b", 200, 2));
  forward.push_back(make_measurement(3, "bus:a", 300, 3));
  std::vector<Measurement> reversed(forward.rbegin(), forward.rend());

  const Result<EvidenceSet> first = EvidenceSet::build(std::move(forward));
  const Result<EvidenceSet> second = EvidenceSet::build(std::move(reversed));
  PO_REQUIRE_OK(first);
  PO_REQUIRE_OK(second);
  PO_CHECK_EQ(first.value().content_hash(), second.value().content_hash());
  PO_CHECK_EQ(first.value().size(), second.value().size());
}

PO_TEST(evidence, identical_construction_yields_identical_hash) {
  std::vector<Measurement> input;
  input.push_back(make_measurement(1, "feed:a", 100, 1));
  const Result<EvidenceSet> first = EvidenceSet::build(input);
  const Result<EvidenceSet> second = EvidenceSet::build(input);
  PO_REQUIRE_OK(first);
  PO_REQUIRE_OK(second);
  PO_CHECK_EQ(first.value().content_hash(), second.value().content_hash());

  std::vector<Measurement> different;
  different.push_back(make_measurement(1, "feed:a", 101, 1));
  const Result<EvidenceSet> third = EvidenceSet::build(std::move(different));
  PO_REQUIRE_OK(third);
  PO_CHECK_NE(first.value().content_hash(), third.value().content_hash());
}

PO_TEST(evidence, structurally_invalid_measurement_is_refused) {
  std::vector<Measurement> input;
  Measurement broken;
  broken.id = MeasurementId(1);
  input.push_back(broken);
  PO_REQUIRE_ERR(EvidenceSet::build(std::move(input)), ReasonCode::InvalidArgument);

  std::vector<Measurement> zero_id;
  zero_id.push_back(make_measurement(0, "feed:a", 1, 1));
  PO_REQUIRE_ERR(EvidenceSet::build(std::move(zero_id)), ReasonCode::InvalidArgument);
}

PO_TEST(evidence, conflicting_meters_are_reported_when_tolerance_is_exceeded) {
  std::vector<Measurement> input;
  input.push_back(make_measurement(1, "feed:a", 100000, 1, 1, "meter-1"));
  input.push_back(make_measurement(2, "feed:a", 130000, 1, 1, "meter-2"));
  const Result<EvidenceSet> set = EvidenceSet::build(std::move(input));
  PO_REQUIRE_OK(set);

  PO_CHECK_EQ(set.value().conflicting(40000).size(), std::size_t{0});
  const std::vector<EvidenceNote> conflicts = set.value().conflicting(1000);
  PO_REQUIRE(!conflicts.empty());
  PO_CHECK_EQ(conflicts.front().code, ReasonCode::ConflictingMeters);
}

PO_TEST(evidence, recovered_provenance_never_carries_a_monotonic_anchor) {
  const Provenance recovered = make_recovered_provenance(SourceId("s"), AuthorityKind::Observed, Generation(3),
                                                         Epoch(1), Sequence(9), std::nullopt,
                                                         Timestamp::from_unix_seconds(500).value());
  PO_CHECK(recovered.recovered());
  PO_CHECK(!recovered.has_monotonic_anchor);
  PO_CHECK(!recovered.can_measure_age_monotonically());
  PO_CHECK_EQ(recovered.origin, EvidenceOrigin::RecoveredFromStore);
  PO_CHECK_EQ(recovered.generation.value(), std::uint64_t{3});
}

PO_TEST(evidence, authority_predicates_separate_observation_from_intent) {
  PO_CHECK(carries_measurement(AuthorityKind::Observed));
  PO_CHECK(carries_measurement(AuthorityKind::Derived));
  PO_CHECK(carries_measurement(AuthorityKind::External));
  PO_CHECK(!carries_measurement(AuthorityKind::Configured));
  PO_CHECK(!carries_measurement(AuthorityKind::Acknowledged));
  PO_CHECK(!carries_measurement(AuthorityKind::Synthetic));
  PO_CHECK(is_first_hand_observation(AuthorityKind::Observed));
  PO_CHECK(!is_first_hand_observation(AuthorityKind::Derived));
  PO_CHECK(is_acknowledgement(AuthorityKind::Acknowledged));
}

PO_TEST(evidence, index_lookups_are_consistent_with_contents) {
  std::vector<Measurement> input;
  input.push_back(make_measurement(1, "feed:a", 100, 1));
  input.push_back(make_measurement(2, "bus:a", 200, 2));
  const Result<EvidenceSet> set = EvidenceSet::build(std::move(input));
  PO_REQUIRE_OK(set);
  PO_CHECK_EQ(set.value().for_entity(EntityRef::feed(FeedId("a"))).size(), std::size_t{1});
  PO_CHECK_EQ(set.value().for_entity(EntityRef::bus(BusId("a"))).size(), std::size_t{1});
  PO_CHECK_EQ(set.value().for_entity(EntityRef::load(LoadId("missing"))).size(), std::size_t{0});
  PO_REQUIRE(set.value().find(MeasurementId(2)) != nullptr);
  PO_CHECK(set.value().find(MeasurementId(99)) == nullptr);
  PO_CHECK_EQ(set.value().sources().size(), std::size_t{1});
  PO_CHECK_EQ(set.value().entities().size(), std::size_t{2});
}

PO_TEST(evidence, fnv_hash_is_stable_and_order_sensitive) {
  PO_CHECK_EQ(fnv1a64(""), Fnv1a64::kOffsetBasis);
  PO_CHECK_EQ(fnv1a64("a"), 0xAF63DC4C8601EC8Cull);
  PO_CHECK_NE(fnv1a64("ab"), fnv1a64("ba"));
  PO_CHECK_EQ(combine_text(1, "a"), combine_text(1, "a"));
  PO_CHECK_NE(combine_text(1, "a"), combine_text(2, "a"));
}

PO_TEST_MAIN()
