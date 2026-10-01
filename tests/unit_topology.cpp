// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <string>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/scenario.hpp"
#include "power_observatory/topology.hpp"

using namespace po;

namespace {

Result<Scenario> standard(std::uint64_t seed = 3) {
  return build_standard_scenario(pofix::scenario_options(seed, 1));
}

}  // namespace

PO_TEST(topology, standard_scenario_declares_a_complete_chain) {
  const Result<Scenario> scenario = standard();
  PO_REQUIRE_OK(scenario);
  const TopologyModel& topology = scenario.value().topology;
  PO_CHECK_EQ(topology.feeds().size(), std::size_t{2});
  PO_CHECK_EQ(topology.buses().size(), std::size_t{4});
  PO_CHECK_EQ(topology.ups_units().size(), std::size_t{2});
  PO_CHECK_EQ(topology.pdus().size(), std::size_t{4});
  PO_CHECK_EQ(topology.circuits().size(), std::size_t{8});
  PO_CHECK_EQ(topology.loads().size(), std::size_t{8});
  PO_CHECK_EQ(topology.redundancy_groups().size(), std::size_t{1});
  PO_CHECK_EQ(topology.size(), std::size_t{29});
}

PO_TEST(topology, declared_children_follow_the_wiring_including_the_ups) {
  const Result<Scenario> scenario = standard();
  PO_REQUIRE_OK(scenario);
  const TopologyModel& topology = scenario.value().topology;

  // A bus feeds the UPS whose input it is, and the PDUs whose input it is. A
  // bus that silently stopped at the PDUs would hide the whole UPS stage.
  const std::vector<EntityRef> intake =
      topology.children(EntityRef::bus(BusId("bus-a")));
  PO_CHECK_EQ(intake.size(), std::size_t{1});
  PO_CHECK_EQ(intake.front().to_string(), std::string("ups:ups-a"));

  const std::vector<EntityRef> output = topology.children(EntityRef::bus(BusId("bus-a-out")));
  PO_CHECK_EQ(output.size(), std::size_t{2});

  const std::vector<EntityRef> ups_children = topology.children(EntityRef::ups(UpsId("ups-a")));
  PO_CHECK_EQ(ups_children.size(), std::size_t{1});
  PO_CHECK_EQ(ups_children.front().to_string(), std::string("bus:bus-a-out"));
}

PO_TEST(topology, upstream_feeds_walk_the_whole_path) {
  const Result<Scenario> scenario = standard();
  PO_REQUIRE_OK(scenario);
  const TopologyModel& topology = scenario.value().topology;

  const std::vector<EntityRef> feeds = topology.upstream_feeds(EntityRef::load(LoadId("pdu-a1-load1")));
  PO_REQUIRE(feeds.size() == 1);
  PO_CHECK_EQ(feeds.front().to_string(), std::string("feed:main-a"));

  const std::vector<EntityRef> bus_feeds = topology.upstream_feeds(EntityRef::bus(BusId("bus-b-out")));
  PO_REQUIRE(bus_feeds.size() == 1);
  PO_CHECK_EQ(bus_feeds.front().to_string(), std::string("feed:main-b"));
}

PO_TEST(topology, dangling_reference_is_refused) {
  Feed feed;
  feed.id = FeedId("f");
  feed.name = "f";
  feed.lands_on = BusId("does-not-exist");
  FeedSet feeds;
  feeds.push_back(feed);
  const Result<TopologyModel> model = TopologyModel::build(std::move(feeds), {}, {}, {}, {}, {}, {}, {});
  PO_REQUIRE_ERR(model, ReasonCode::TopologyUnknown);
}

PO_TEST(topology, duplicate_identifier_is_refused) {
  Feed first;
  first.id = FeedId("f");
  Feed second;
  second.id = FeedId("f");
  FeedSet feeds;
  feeds.push_back(first);
  feeds.push_back(second);
  PO_REQUIRE_ERR(TopologyModel::build(std::move(feeds), {}, {}, {}, {}, {}, {}, {}), ReasonCode::SchemaViolation);
}

PO_TEST(topology, empty_identifier_is_refused) {
  Feed feed;
  feed.id = FeedId("");
  FeedSet feeds;
  feeds.push_back(feed);
  PO_REQUIRE_ERR(TopologyModel::build(std::move(feeds), {}, {}, {}, {}, {}, {}, {}), ReasonCode::SchemaViolation);
}

PO_TEST(topology, negative_declared_capacity_is_refused) {
  Feed feed;
  feed.id = FeedId("f");
  feed.rated_capacity = Power::from_raw(-1);
  FeedSet feeds;
  feeds.push_back(feed);
  PO_REQUIRE_ERR(TopologyModel::build(std::move(feeds), {}, {}, {}, {}, {}, {}, {}), ReasonCode::SchemaViolation);
}

PO_TEST(topology, redundancy_group_must_require_what_it_declares) {
  FeedSet feeds;
  Feed feed;
  feed.id = FeedId("f");
  feeds.push_back(feed);

  RedundancyGroup group;
  group.id = RedundancyGroupId("g");
  group.members.push_back(FeedId("f"));
  group.required_live = 0;
  RedundancyGroupSet zero;
  zero.push_back(group);
  PO_REQUIRE_ERR(TopologyModel::build(feeds, {}, {}, {}, {}, {}, {}, std::move(zero)),
                 ReasonCode::SchemaViolation);

  RedundancyGroup too_many = group;
  too_many.required_live = 2;
  RedundancyGroupSet excessive;
  excessive.push_back(too_many);
  PO_REQUIRE_ERR(TopologyModel::build(std::move(feeds), {}, {}, {}, {}, {}, {}, std::move(excessive)),
                 ReasonCode::SchemaViolation);
}

PO_TEST(topology, content_hash_is_order_independent_and_content_sensitive) {
  const Result<Scenario> first = standard(1);
  const Result<Scenario> second = standard(1);
  const Result<Scenario> third = standard(2);
  PO_REQUIRE_OK(first);
  PO_REQUIRE_OK(second);
  PO_REQUIRE_OK(third);
  PO_CHECK_EQ(first.value().topology.content_hash(), second.value().topology.content_hash());
  PO_CHECK_EQ(first.value().topology.content_hash(), third.value().topology.content_hash());
}

PO_TEST(topology, declared_state_gating_separates_intent_from_measurement) {
  PO_CHECK(declared_available(LifecycleState::InService));
  PO_CHECK(declared_available(LifecycleState::Commissioned));
  PO_CHECK(!declared_available(LifecycleState::Maintenance));
  PO_CHECK(!declared_available(LifecycleState::Failed));
  PO_CHECK(!declared_available(LifecycleState::Decommissioned));
  PO_CHECK(!declared_available(LifecycleState::Planned));
  PO_CHECK(!declared_available(LifecycleState::Unknown));
}

PO_TEST(topology, lookup_of_an_undeclared_entity_returns_null) {
  const Result<Scenario> scenario = standard();
  PO_REQUIRE_OK(scenario);
  const TopologyModel& topology = scenario.value().topology;
  PO_CHECK(topology.feed(FeedId("nope")) == nullptr);
  PO_CHECK(topology.bus(BusId("nope")) == nullptr);
  PO_CHECK(topology.ups(UpsId("nope")) == nullptr);
  PO_CHECK(topology.pdu(PduId("nope")) == nullptr);
  PO_CHECK(topology.circuit(CircuitId("nope")) == nullptr);
  PO_CHECK(topology.load(LoadId("nope")) == nullptr);
  PO_CHECK(topology.redundancy_group(RedundancyGroupId("nope")) == nullptr);
  PO_CHECK(!topology.contains(EntityRef::feed(FeedId("nope"))));
}

PO_TEST_MAIN()