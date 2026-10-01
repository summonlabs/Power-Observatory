// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "power_observatory/evidence.hpp"
#include "power_observatory/quantity.hpp"
#include "power_observatory/result.hpp"
#include "power_observatory/strong.hpp"

namespace po {

// Declared electrical topology.
//
// Every element in this model is Configured authority: it records what the
// plant is believed to be wired as, not what it is currently doing. No value in
// this model is ever presented as observed electrical state, and no observation
// is ever inferred from it. Freshness of a topology element describes how
// current the declaration is, nothing more.
enum class RedundancyTopology : std::uint8_t {
  Unknown = 0,
  None = 1,
  N = 2,
  NPlusOne = 3,
  TwoN = 4,
  TwoNPlusOne = 5,
  DistributedRedundant = 6,
};

enum class Criticality : std::uint8_t {
  Unknown = 0,
  Low = 1,
  Medium = 2,
  High = 3,
  MissionCritical = 4,
};

// Declared lifecycle. "InService" is a declaration; it is not evidence that the
// element is carrying current.
enum class LifecycleState : std::uint8_t {
  Unknown = 0,
  Planned = 1,
  Commissioned = 2,
  InService = 3,
  Maintenance = 4,
  Failed = 5,
  Decommissioned = 6,
};

[[nodiscard]] std::string_view to_string(RedundancyTopology topology) noexcept;
[[nodiscard]] std::string_view to_string(Criticality criticality) noexcept;
[[nodiscard]] std::string_view to_string(LifecycleState state) noexcept;

// True when the declaration permits the element to carry load. This is a
// filter over declarations, never a measurement.
[[nodiscard]] constexpr bool declared_available(LifecycleState state) noexcept {
  return state == LifecycleState::InService || state == LifecycleState::Commissioned;
}

struct Feed {
  FeedId id;
  std::string name;
  std::optional<BusId> lands_on;
  Power rated_capacity{};
  RedundancyGroupId group{};
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

struct Bus {
  BusId id;
  std::string name;
  std::vector<FeedId> fed_by;
  Power rated_capacity{};
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

struct Ups {
  UpsId id;
  std::string name;
  std::optional<BusId> input;
  std::vector<BusId> outputs;
  Power rated_capacity{};
  // Declared conversion efficiency. Used only as a fallback when no measured
  // input and output are available, and any conclusion drawn from it is marked
  // as derived from configuration.
  Ratio declared_efficiency_ppm{Ratio::from_raw(1000000)};
  Duration declared_autonomy{};
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

struct Generator {
  GeneratorId id;
  std::string name;
  std::optional<BusId> lands_on;
  Power rated_capacity{};
  Duration declared_autonomy{};
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

struct Pdu {
  PduId id;
  std::string name;
  std::optional<BusId> input;
  Power rated_capacity{};
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

struct Circuit {
  CircuitId id;
  std::string name;
  std::optional<PduId> pdu;
  Power rated_capacity{};
  std::optional<LoadId> serves;
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

struct Load {
  LoadId id;
  std::string name;
  std::optional<CircuitId> circuit;
  Power rated_capacity{};
  Criticality criticality{Criticality::Unknown};
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

struct RedundancyGroup {
  RedundancyGroupId id;
  std::string name;
  RedundancyTopology topology{RedundancyTopology::Unknown};
  std::vector<FeedId> members;
  // How many members must be simultaneously live for the group to retain its
  // declared redundancy.
  std::uint32_t required_live{1};
  // Declared transfer time. Configured authority: it states what the design
  // intends, not what a transfer actually took.
  Duration declared_transfer_time{};
  LifecycleState declared_state{LifecycleState::Unknown};
  Provenance declared_by;
};

using FeedSet = std::vector<Feed>;
using BusSet = std::vector<Bus>;
using UpsSet = std::vector<Ups>;
using GeneratorSet = std::vector<Generator>;
using PduSet = std::vector<Pdu>;
using CircuitSet = std::vector<Circuit>;
using LoadSet = std::vector<Load>;
using RedundancyGroupSet = std::vector<RedundancyGroup>;

// Validated, indexed topology. Construction rejects structurally impossible
// declarations (dangling references, duplicate identifiers, zero identifiers)
// so that later stages can rely on referential integrity.
class TopologyModel {
 public:
  TopologyModel() = default;

  [[nodiscard]] static Result<TopologyModel> build(FeedSet feeds, BusSet buses, UpsSet ups, GeneratorSet generators,
                                                   PduSet pdus, CircuitSet circuits, LoadSet loads,
                                                   RedundancyGroupSet groups);

  [[nodiscard]] const Feed* feed(const FeedId& id) const;
  [[nodiscard]] const Bus* bus(const BusId& id) const;
  [[nodiscard]] const Ups* ups(const UpsId& id) const;
  [[nodiscard]] const Generator* generator(const GeneratorId& id) const;
  [[nodiscard]] const Pdu* pdu(const PduId& id) const;
  [[nodiscard]] const Circuit* circuit(const CircuitId& id) const;
  [[nodiscard]] const Load* load(const LoadId& id) const;
  [[nodiscard]] const RedundancyGroup* redundancy_group(const RedundancyGroupId& id) const;

  // Generic lookup used by the engines, which work over EntityRef.
  [[nodiscard]] bool contains(const EntityRef& entity) const;
  [[nodiscard]] Power declared_capacity(const EntityRef& entity) const;
  [[nodiscard]] LifecycleState declared_state(const EntityRef& entity) const;
  [[nodiscard]] std::string display_name(const EntityRef& entity) const;

  // Direct children of an entity in the declared wiring. Deterministic order.
  [[nodiscard]] std::vector<EntityRef> children(const EntityRef& entity) const;
  // The feed(s) whose declared path reaches this entity.
  [[nodiscard]] std::vector<EntityRef> upstream_feeds(const EntityRef& entity) const;

  [[nodiscard]] const std::vector<Feed>& feeds() const noexcept { return feeds_; }
  [[nodiscard]] const std::vector<Bus>& buses() const noexcept { return buses_; }
  [[nodiscard]] const std::vector<Ups>& ups_units() const noexcept { return ups_; }
  [[nodiscard]] const std::vector<Generator>& generators() const noexcept { return generators_; }
  [[nodiscard]] const std::vector<Pdu>& pdus() const noexcept { return pdus_; }
  [[nodiscard]] const std::vector<Circuit>& circuits() const noexcept { return circuits_; }
  [[nodiscard]] const std::vector<Load>& loads() const noexcept { return loads_; }
  [[nodiscard]] const std::vector<RedundancyGroup>& redundancy_groups() const noexcept { return groups_; }
  [[nodiscard]] const std::vector<EntityRef>& entity_order() const noexcept { return entity_order_; }

  [[nodiscard]] bool empty() const noexcept { return entity_order_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return entity_order_.size(); }
  [[nodiscard]] std::uint64_t content_hash() const noexcept { return content_hash_; }

 private:
  std::vector<Feed> feeds_;
  std::vector<Bus> buses_;
  std::vector<Ups> ups_;
  std::vector<Generator> generators_;
  std::vector<Pdu> pdus_;
  std::vector<Circuit> circuits_;
  std::vector<Load> loads_;
  std::vector<RedundancyGroup> groups_;
  std::map<EntityRef, std::size_t> index_;
  std::vector<EntityRef> entity_order_;
  std::uint64_t content_hash_{0};
};

}  // namespace po
