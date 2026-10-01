// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/topology.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <string_view>

#include "power_observatory/hashing.hpp"

namespace po {

std::string_view to_string(RedundancyTopology topology) noexcept {
  switch (topology) {
    case RedundancyTopology::Unknown:
      return "unknown";
    case RedundancyTopology::None:
      return "none";
    case RedundancyTopology::N:
      return "n";
    case RedundancyTopology::NPlusOne:
      return "n_plus_one";
    case RedundancyTopology::TwoN:
      return "two_n";
    case RedundancyTopology::TwoNPlusOne:
      return "two_n_plus_one";
    case RedundancyTopology::DistributedRedundant:
      return "distributed_redundant";
  }
  return "unknown";
}

std::string_view to_string(Criticality criticality) noexcept {
  switch (criticality) {
    case Criticality::Unknown:
      return "unknown";
    case Criticality::Low:
      return "low";
    case Criticality::Medium:
      return "medium";
    case Criticality::High:
      return "high";
    case Criticality::MissionCritical:
      return "mission_critical";
  }
  return "unknown";
}

std::string_view to_string(LifecycleState state) noexcept {
  switch (state) {
    case LifecycleState::Unknown:
      return "unknown";
    case LifecycleState::Planned:
      return "planned";
    case LifecycleState::Commissioned:
      return "commissioned";
    case LifecycleState::InService:
      return "in_service";
    case LifecycleState::Maintenance:
      return "maintenance";
    case LifecycleState::Failed:
      return "failed";
    case LifecycleState::Decommissioned:
      return "decommissioned";
  }
  return "unknown";
}

namespace {

template <class T, class IdOf>
[[nodiscard]] Status sort_and_check_duplicates(std::vector<T>& items, IdOf id_of, std::string_view what) {
  std::sort(items.begin(), items.end(), [&id_of](const T& left, const T& right) { return id_of(left) < id_of(right); });
  for (std::size_t index = 1; index < items.size(); ++index) {
    if (id_of(items[index - 1]) == id_of(items[index])) {
      return fail(ReasonCode::SchemaViolation,
                  std::string(what) + " declares identifier '" + id_of(items[index]).value() + "' more than once");
    }
  }
  return ok_status();
}

template <class IdOf>
[[nodiscard]] Status check_empty_ids(const auto& items, IdOf id_of, std::string_view what) {
  for (const auto& item : items) {
    if (id_of(item).empty()) {
      return fail(ReasonCode::SchemaViolation, std::string(what) + " contains an entry with an empty identifier");
    }
  }
  return ok_status();
}

[[nodiscard]] std::vector<EntityRef> parents_of(const TopologyModel& model, const EntityRef& entity) {
  std::vector<EntityRef> result;
  switch (entity.kind()) {
    case EntityKind::Bus: {
      const BusId bus_id(entity.id());
      for (const Feed& candidate : model.feeds()) {
        if (candidate.lands_on && *candidate.lands_on == bus_id) {
          result.push_back(EntityRef::feed(candidate.id));
        }
      }
      for (const Ups& candidate : model.ups_units()) {
        for (const BusId& output : candidate.outputs) {
          if (output == bus_id) {
            result.push_back(EntityRef::ups(candidate.id));
          }
        }
      }
      break;
    }
    case EntityKind::Ups: {
      const Ups* unit = model.ups(UpsId(entity.id()));
      if (unit != nullptr && unit->input.has_value()) {
        result.push_back(EntityRef::bus(*unit->input));
      }
      break;
    }
    case EntityKind::Pdu: {
      const Pdu* unit = model.pdu(PduId(entity.id()));
      if (unit != nullptr && unit->input.has_value()) {
        result.push_back(EntityRef::bus(*unit->input));
      }
      break;
    }
    case EntityKind::Circuit: {
      const Circuit* unit = model.circuit(CircuitId(entity.id()));
      if (unit != nullptr && unit->pdu.has_value()) {
        result.push_back(EntityRef::pdu(*unit->pdu));
      }
      break;
    }
    case EntityKind::Load: {
      const Load* unit = model.load(LoadId(entity.id()));
      if (unit != nullptr && unit->circuit.has_value()) {
        result.push_back(EntityRef::circuit(*unit->circuit));
      }
      break;
    }
    case EntityKind::Unknown:
    case EntityKind::Feed:
    case EntityKind::Generator:
    case EntityKind::RedundancyGroup:
      break;
  }
  return result;
}

}  // namespace

Result<TopologyModel> TopologyModel::build(FeedSet feeds, BusSet buses, UpsSet ups, GeneratorSet generators,
                                           PduSet pdus, CircuitSet circuits, LoadSet loads,
                                           RedundancyGroupSet groups) {
  Status status = sort_and_check_duplicates(feeds, [](const Feed& entry) { return entry.id; }, "feeds");
  if (!status) {
    return status.error();
  }
  status = sort_and_check_duplicates(buses, [](const Bus& entry) { return entry.id; }, "buses");
  if (!status) {
    return status.error();
  }
  status = sort_and_check_duplicates(ups, [](const Ups& entry) { return entry.id; }, "ups units");
  if (!status) {
    return status.error();
  }
  status = sort_and_check_duplicates(generators, [](const Generator& entry) { return entry.id; }, "generators");
  if (!status) {
    return status.error();
  }
  status = sort_and_check_duplicates(pdus, [](const Pdu& entry) { return entry.id; }, "pdus");
  if (!status) {
    return status.error();
  }
  status = sort_and_check_duplicates(circuits, [](const Circuit& entry) { return entry.id; }, "circuits");
  if (!status) {
    return status.error();
  }
  status = sort_and_check_duplicates(loads, [](const Load& entry) { return entry.id; }, "loads");
  if (!status) {
    return status.error();
  }
  status = sort_and_check_duplicates(groups, [](const RedundancyGroup& entry) { return entry.id; },
                                     "redundancy groups");
  if (!status) {
    return status.error();
  }

  status = check_empty_ids(feeds, [](const Feed& entry) { return entry.id; }, "feeds");
  if (!status) {
    return status.error();
  }
  status = check_empty_ids(buses, [](const Bus& entry) { return entry.id; }, "buses");
  if (!status) {
    return status.error();
  }
  status = check_empty_ids(ups, [](const Ups& entry) { return entry.id; }, "ups units");
  if (!status) {
    return status.error();
  }
  status = check_empty_ids(generators, [](const Generator& entry) { return entry.id; }, "generators");
  if (!status) {
    return status.error();
  }
  status = check_empty_ids(pdus, [](const Pdu& entry) { return entry.id; }, "pdus");
  if (!status) {
    return status.error();
  }
  status = check_empty_ids(circuits, [](const Circuit& entry) { return entry.id; }, "circuits");
  if (!status) {
    return status.error();
  }
  status = check_empty_ids(loads, [](const Load& entry) { return entry.id; }, "loads");
  if (!status) {
    return status.error();
  }
  status = check_empty_ids(groups, [](const RedundancyGroup& entry) { return entry.id; }, "redundancy groups");
  if (!status) {
    return status.error();
  }

  TopologyModel model;
  model.feeds_ = std::move(feeds);
  model.buses_ = std::move(buses);
  model.ups_ = std::move(ups);
  model.generators_ = std::move(generators);
  model.pdus_ = std::move(pdus);
  model.circuits_ = std::move(circuits);
  model.loads_ = std::move(loads);
  model.groups_ = std::move(groups);

  for (std::size_t index = 0; index < model.feeds_.size(); ++index) {
    model.index_.emplace(EntityRef::feed(model.feeds_[index].id), index);
  }
  for (std::size_t index = 0; index < model.buses_.size(); ++index) {
    model.index_.emplace(EntityRef::bus(model.buses_[index].id), index);
  }
  for (std::size_t index = 0; index < model.ups_.size(); ++index) {
    model.index_.emplace(EntityRef::ups(model.ups_[index].id), index);
  }
  for (std::size_t index = 0; index < model.generators_.size(); ++index) {
    model.index_.emplace(EntityRef::generator(model.generators_[index].id), index);
  }
  for (std::size_t index = 0; index < model.pdus_.size(); ++index) {
    model.index_.emplace(EntityRef::pdu(model.pdus_[index].id), index);
  }
  for (std::size_t index = 0; index < model.circuits_.size(); ++index) {
    model.index_.emplace(EntityRef::circuit(model.circuits_[index].id), index);
  }
  for (std::size_t index = 0; index < model.loads_.size(); ++index) {
    model.index_.emplace(EntityRef::load(model.loads_[index].id), index);
  }
  for (std::size_t index = 0; index < model.groups_.size(); ++index) {
    model.index_.emplace(EntityRef::redundancy_group(model.groups_[index].id), index);
  }
  for (const auto& entry : model.index_) {
    model.entity_order_.push_back(entry.first);
  }

  const auto require = [&model](const EntityRef& reference, const std::string& context) -> Status {
    if (!model.contains(reference)) {
      return fail(ReasonCode::TopologyUnknown, context + " refers to unknown entity " + reference.to_string());
    }
    return ok_status();
  };

  for (const Feed& entry : model.feeds_) {
    const std::string context = "feed '" + entry.id.value() + "'";
    if (entry.lands_on.has_value()) {
      status = require(EntityRef::bus(*entry.lands_on), context);
      if (!status) {
        return status.error();
      }
    }
    if (!entry.group.empty()) {
      status = require(EntityRef::redundancy_group(entry.group), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.rated_capacity.is_negative()) {
      return Error(ReasonCode::SchemaViolation, context + " declares a negative rated capacity");
    }
  }

  for (const Bus& entry : model.buses_) {
    const std::string context = "bus '" + entry.id.value() + "'";
    for (const FeedId& feed_id : entry.fed_by) {
      status = require(EntityRef::feed(feed_id), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.rated_capacity.is_negative()) {
      return Error(ReasonCode::SchemaViolation, context + " declares a negative rated capacity");
    }
  }

  for (const Ups& entry : model.ups_) {
    const std::string context = "ups '" + entry.id.value() + "'";
    if (entry.input.has_value()) {
      status = require(EntityRef::bus(*entry.input), context);
      if (!status) {
        return status.error();
      }
    }
    for (const BusId& output : entry.outputs) {
      status = require(EntityRef::bus(output), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.rated_capacity.is_negative()) {
      return Error(ReasonCode::SchemaViolation, context + " declares a negative rated capacity");
    }
    if (entry.declared_efficiency_ppm.raw() < 0 || entry.declared_efficiency_ppm.raw() > 1000000) {
      return Error(ReasonCode::SchemaViolation, context + " declares an efficiency outside 0%..100%");
    }
  }

  for (const Generator& entry : model.generators_) {
    const std::string context = "generator '" + entry.id.value() + "'";
    if (entry.lands_on.has_value()) {
      status = require(EntityRef::bus(*entry.lands_on), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.rated_capacity.is_negative()) {
      return Error(ReasonCode::SchemaViolation, context + " declares a negative rated capacity");
    }
  }

  for (const Pdu& entry : model.pdus_) {
    const std::string context = "pdu '" + entry.id.value() + "'";
    if (entry.input.has_value()) {
      status = require(EntityRef::bus(*entry.input), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.rated_capacity.is_negative()) {
      return Error(ReasonCode::SchemaViolation, context + " declares a negative rated capacity");
    }
  }

  for (const Circuit& entry : model.circuits_) {
    const std::string context = "circuit '" + entry.id.value() + "'";
    if (entry.pdu.has_value()) {
      status = require(EntityRef::pdu(*entry.pdu), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.serves.has_value()) {
      status = require(EntityRef::load(*entry.serves), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.rated_capacity.is_negative()) {
      return Error(ReasonCode::SchemaViolation, context + " declares a negative rated capacity");
    }
  }

  for (const Load& entry : model.loads_) {
    const std::string context = "load '" + entry.id.value() + "'";
    if (entry.circuit.has_value()) {
      status = require(EntityRef::circuit(*entry.circuit), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.rated_capacity.is_negative()) {
      return Error(ReasonCode::SchemaViolation, context + " declares a negative rated capacity");
    }
  }

  for (const RedundancyGroup& entry : model.groups_) {
    const std::string context = "redundancy group '" + entry.id.value() + "'";
    if (entry.members.empty()) {
      return Error(ReasonCode::SchemaViolation, context + " declares no members");
    }
    for (const FeedId& member : entry.members) {
      status = require(EntityRef::feed(member), context);
      if (!status) {
        return status.error();
      }
    }
    if (entry.required_live == 0) {
      return Error(ReasonCode::SchemaViolation, context + " requires zero live members");
    }
    if (static_cast<std::size_t>(entry.required_live) > entry.members.size()) {
      return Error(ReasonCode::SchemaViolation,
                   context + " requires more live members (" + std::to_string(entry.required_live) +
                       ") than it declares (" + std::to_string(entry.members.size()) + ")");
    }
  }

  Fnv1a64 hasher;
  for (const EntityRef& entity : model.entity_order_) {
    hasher.update(entity.to_string());
    hasher.update_separator('n');
    hasher.update(model.display_name(entity));
    hasher.update_separator('c');
    hasher.update_integral(model.declared_capacity(entity).raw());
    hasher.update_separator('l');
    hasher.update_integral(static_cast<std::uint8_t>(model.declared_state(entity)));
    hasher.update_separator('>');
    for (const EntityRef& child : model.children(entity)) {
      hasher.update(child.to_string());
    }
  }
  model.content_hash_ = hasher.value();
  return model;
}

const Feed* TopologyModel::feed(const FeedId& id) const {
  const auto found = index_.find(EntityRef::feed(id));
  if (found == index_.end() || found->second >= feeds_.size()) {
    return nullptr;
  }
  return &feeds_[found->second];
}

const Bus* TopologyModel::bus(const BusId& id) const {
  const auto found = index_.find(EntityRef::bus(id));
  if (found == index_.end() || found->second >= buses_.size()) {
    return nullptr;
  }
  return &buses_[found->second];
}

const Ups* TopologyModel::ups(const UpsId& id) const {
  const auto found = index_.find(EntityRef::ups(id));
  if (found == index_.end() || found->second >= ups_.size()) {
    return nullptr;
  }
  return &ups_[found->second];
}

const Generator* TopologyModel::generator(const GeneratorId& id) const {
  const auto found = index_.find(EntityRef::generator(id));
  if (found == index_.end() || found->second >= generators_.size()) {
    return nullptr;
  }
  return &generators_[found->second];
}

const Pdu* TopologyModel::pdu(const PduId& id) const {
  const auto found = index_.find(EntityRef::pdu(id));
  if (found == index_.end() || found->second >= pdus_.size()) {
    return nullptr;
  }
  return &pdus_[found->second];
}

const Circuit* TopologyModel::circuit(const CircuitId& id) const {
  const auto found = index_.find(EntityRef::circuit(id));
  if (found == index_.end() || found->second >= circuits_.size()) {
    return nullptr;
  }
  return &circuits_[found->second];
}

const Load* TopologyModel::load(const LoadId& id) const {
  const auto found = index_.find(EntityRef::load(id));
  if (found == index_.end() || found->second >= loads_.size()) {
    return nullptr;
  }
  return &loads_[found->second];
}

const RedundancyGroup* TopologyModel::redundancy_group(const RedundancyGroupId& id) const {
  const auto found = index_.find(EntityRef::redundancy_group(id));
  if (found == index_.end() || found->second >= groups_.size()) {
    return nullptr;
  }
  return &groups_[found->second];
}

bool TopologyModel::contains(const EntityRef& entity) const { return index_.find(entity) != index_.end(); }

Power TopologyModel::declared_capacity(const EntityRef& entity) const {
  switch (entity.kind()) {
    case EntityKind::Feed: {
      const Feed* entry = feed(FeedId(entity.id()));
      return entry == nullptr ? Power{} : entry->rated_capacity;
    }
    case EntityKind::Bus: {
      const Bus* entry = bus(BusId(entity.id()));
      return entry == nullptr ? Power{} : entry->rated_capacity;
    }
    case EntityKind::Ups: {
      const Ups* entry = ups(UpsId(entity.id()));
      return entry == nullptr ? Power{} : entry->rated_capacity;
    }
    case EntityKind::Generator: {
      const Generator* entry = generator(GeneratorId(entity.id()));
      return entry == nullptr ? Power{} : entry->rated_capacity;
    }
    case EntityKind::Pdu: {
      const Pdu* entry = pdu(PduId(entity.id()));
      return entry == nullptr ? Power{} : entry->rated_capacity;
    }
    case EntityKind::Circuit: {
      const Circuit* entry = circuit(CircuitId(entity.id()));
      return entry == nullptr ? Power{} : entry->rated_capacity;
    }
    case EntityKind::Load: {
      const Load* entry = load(LoadId(entity.id()));
      return entry == nullptr ? Power{} : entry->rated_capacity;
    }
    case EntityKind::Unknown:
    case EntityKind::RedundancyGroup:
      return Power{};
  }
  return Power{};
}

LifecycleState TopologyModel::declared_state(const EntityRef& entity) const {
  switch (entity.kind()) {
    case EntityKind::Feed: {
      const Feed* entry = feed(FeedId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::Bus: {
      const Bus* entry = bus(BusId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::Ups: {
      const Ups* entry = ups(UpsId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::Generator: {
      const Generator* entry = generator(GeneratorId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::Pdu: {
      const Pdu* entry = pdu(PduId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::Circuit: {
      const Circuit* entry = circuit(CircuitId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::Load: {
      const Load* entry = load(LoadId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::RedundancyGroup: {
      const RedundancyGroup* entry = redundancy_group(RedundancyGroupId(entity.id()));
      return entry == nullptr ? LifecycleState::Unknown : entry->declared_state;
    }
    case EntityKind::Unknown:
      return LifecycleState::Unknown;
  }
  return LifecycleState::Unknown;
}

std::string TopologyModel::display_name(const EntityRef& entity) const {
  switch (entity.kind()) {
    case EntityKind::Feed: {
      const Feed* entry = feed(FeedId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::Bus: {
      const Bus* entry = bus(BusId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::Ups: {
      const Ups* entry = ups(UpsId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::Generator: {
      const Generator* entry = generator(GeneratorId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::Pdu: {
      const Pdu* entry = pdu(PduId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::Circuit: {
      const Circuit* entry = circuit(CircuitId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::Load: {
      const Load* entry = load(LoadId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::RedundancyGroup: {
      const RedundancyGroup* entry = redundancy_group(RedundancyGroupId(entity.id()));
      return entry == nullptr ? entity.id() : entry->name;
    }
    case EntityKind::Unknown:
      return entity.id();
  }
  return entity.id();
}

std::vector<EntityRef> TopologyModel::children(const EntityRef& entity) const {
  std::vector<EntityRef> result;
  switch (entity.kind()) {
    case EntityKind::RedundancyGroup: {
      const RedundancyGroup* group = redundancy_group(RedundancyGroupId(entity.id()));
      if (group != nullptr) {
        for (const FeedId& member : group->members) {
          result.push_back(EntityRef::feed(member));
        }
      }
      break;
    }
    case EntityKind::Feed: {
      const Feed* entry = feed(FeedId(entity.id()));
      if (entry != nullptr && entry->lands_on.has_value()) {
        result.push_back(EntityRef::bus(*entry->lands_on));
      }
      break;
    }
    case EntityKind::Bus: {
      const BusId bus_id(entity.id());
      for (const Ups& entry : ups_) {
        if (entry.input.has_value() && *entry.input == bus_id) {
          result.push_back(EntityRef::ups(entry.id));
        }
      }
      for (const Pdu& entry : pdus_) {
        if (entry.input.has_value() && *entry.input == bus_id) {
          result.push_back(EntityRef::pdu(entry.id));
        }
      }
      break;
    }
    case EntityKind::Ups: {
      const Ups* entry = ups(UpsId(entity.id()));
      if (entry != nullptr) {
        for (const BusId& output : entry->outputs) {
          result.push_back(EntityRef::bus(output));
        }
      }
      break;
    }
    case EntityKind::Pdu: {
      const PduId pdu_id(entity.id());
      for (const Circuit& entry : circuits_) {
        if (entry.pdu.has_value() && *entry.pdu == pdu_id) {
          result.push_back(EntityRef::circuit(entry.id));
        }
      }
      break;
    }
    case EntityKind::Circuit: {
      const Circuit* entry = circuit(CircuitId(entity.id()));
      if (entry != nullptr && entry->serves.has_value()) {
        result.push_back(EntityRef::load(*entry->serves));
      }
      break;
    }
    case EntityKind::Generator: {
      const Generator* entry = generator(GeneratorId(entity.id()));
      if (entry != nullptr && entry->lands_on.has_value()) {
        result.push_back(EntityRef::bus(*entry->lands_on));
      }
      break;
    }
    case EntityKind::Unknown:
    case EntityKind::Load:
      break;
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::vector<EntityRef> TopologyModel::upstream_feeds(const EntityRef& entity) const {
  std::vector<EntityRef> result;
  std::vector<EntityRef> pending;
  std::set<EntityRef> visited;
  pending.push_back(entity);

  // The traversal is bounded by the number of declared entities, so a topology
  // that somehow contains a cycle still terminates instead of hanging.
  const std::size_t budget = (entity_order_.size() + 1) * 2;
  std::size_t steps = 0;
  while (!pending.empty() && steps < budget) {
    ++steps;
    const EntityRef current = pending.back();
    pending.pop_back();
    if (!visited.insert(current).second) {
      continue;
    }
    if (current.kind() == EntityKind::Feed) {
      result.push_back(current);
      continue;
    }
    for (const EntityRef& parent : parents_of(*this, current)) {
      pending.push_back(parent);
    }
  }

  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

}  // namespace po
