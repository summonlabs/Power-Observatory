// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/evidence.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>

#include "power_observatory/hashing.hpp"

namespace po {
namespace {

struct KindName {
  EntityKind kind;
  std::string_view name;
};

constexpr std::array<KindName, 8> kEntityKindNames = {{
    {EntityKind::Feed, "feed"},
    {EntityKind::Bus, "bus"},
    {EntityKind::Ups, "ups"},
    {EntityKind::Generator, "generator"},
    {EntityKind::Pdu, "pdu"},
    {EntityKind::Circuit, "circuit"},
    {EntityKind::Load, "load"},
    {EntityKind::RedundancyGroup, "redundancy_group"},
}};

[[nodiscard]] bool is_power_kind(MeasurementKind kind) noexcept {
  return kind == MeasurementKind::ActivePower || kind == MeasurementKind::ApparentPower ||
         kind == MeasurementKind::ReactivePower || kind == MeasurementKind::Energy;
}

}  // namespace

std::string_view to_string(AuthorityKind kind) noexcept {
  switch (kind) {
    case AuthorityKind::Unknown:
      return "unknown";
    case AuthorityKind::Observed:
      return "observed";
    case AuthorityKind::Derived:
      return "derived";
    case AuthorityKind::Configured:
      return "configured";
    case AuthorityKind::Acknowledged:
      return "acknowledged";
    case AuthorityKind::External:
      return "external";
    case AuthorityKind::Synthetic:
      return "synthetic";
  }
  return "unknown";
}

std::string_view to_string(EvidenceOrigin origin) noexcept {
  switch (origin) {
    case EvidenceOrigin::LiveIngest:
      return "live_ingest";
    case EvidenceOrigin::RecoveredFromStore:
      return "recovered_from_store";
    case EvidenceOrigin::SyntheticScenario:
      return "synthetic_scenario";
  }
  return "live_ingest";
}

std::string_view to_string(EntityKind kind) noexcept {
  switch (kind) {
    case EntityKind::Unknown:
      return "unknown";
    case EntityKind::Feed:
      return "feed";
    case EntityKind::Bus:
      return "bus";
    case EntityKind::Ups:
      return "ups";
    case EntityKind::Generator:
      return "generator";
    case EntityKind::Pdu:
      return "pdu";
    case EntityKind::Circuit:
      return "circuit";
    case EntityKind::Load:
      return "load";
    case EntityKind::RedundancyGroup:
      return "redundancy_group";
  }
  return "unknown";
}

std::string_view to_string(MeasurementKind kind) noexcept {
  switch (kind) {
    case MeasurementKind::ActivePower:
      return "active_power";
    case MeasurementKind::ApparentPower:
      return "apparent_power";
    case MeasurementKind::ReactivePower:
      return "reactive_power";
    case MeasurementKind::Voltage:
      return "voltage";
    case MeasurementKind::Current:
      return "current";
    case MeasurementKind::Frequency:
      return "frequency";
    case MeasurementKind::PowerFactor:
      return "power_factor";
    case MeasurementKind::Energy:
      return "energy";
    case MeasurementKind::Temperature:
      return "temperature";
  }
  return "active_power";
}

std::string_view to_string(Phase phase) noexcept {
  switch (phase) {
    case Phase::Total:
      return "total";
    case Phase::A:
      return "a";
    case Phase::B:
      return "b";
    case Phase::C:
      return "c";
    case Phase::AB:
      return "ab";
    case Phase::BC:
      return "bc";
    case Phase::CA:
      return "ca";
    case Phase::Neutral:
      return "neutral";
    case Phase::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string_view to_string(FreshnessClass classification) noexcept {
  switch (classification) {
    case FreshnessClass::Unknown:
      return "unknown";
    case FreshnessClass::Fresh:
      return "fresh";
    case FreshnessClass::Recovered:
      return "recovered";
    case FreshnessClass::Aging:
      return "aging";
    case FreshnessClass::Stale:
      return "stale";
    case FreshnessClass::Expired:
      return "expired";
    case FreshnessClass::NotApplicable:
      return "not_applicable";
  }
  return "unknown";
}

std::string_view to_string(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Unknown:
      return "unknown";
    case EvidenceState::Known:
      return "known";
    case EvidenceState::Recovered:
      return "recovered";
    case EvidenceState::Stale:
      return "stale";
    case EvidenceState::Conflicting:
      return "conflicting";
    case EvidenceState::Indeterminate:
      return "indeterminate";
    case EvidenceState::Unsupported:
      return "unsupported";
    case EvidenceState::Refused:
      return "refused";
    case EvidenceState::Expired:
      return "expired";
  }
  return "unknown";
}

EvidenceState to_evidence_state(FreshnessClass classification) noexcept {
  switch (classification) {
    case FreshnessClass::Fresh:
      return EvidenceState::Known;
    case FreshnessClass::Recovered:
      return EvidenceState::Recovered;
    case FreshnessClass::Aging:
      return EvidenceState::Stale;
    case FreshnessClass::Stale:
      return EvidenceState::Stale;
    case FreshnessClass::Expired:
      return EvidenceState::Expired;
    case FreshnessClass::NotApplicable:
      return EvidenceState::Unsupported;
    case FreshnessClass::Unknown:
      return EvidenceState::Unknown;
  }
  return EvidenceState::Unknown;
}

int evidence_state_rank(EvidenceState state) noexcept {
  switch (state) {
    case EvidenceState::Known:
      return 0;
    case EvidenceState::Recovered:
      return 1;
    case EvidenceState::Stale:
      return 2;
    case EvidenceState::Unknown:
      return 3;
    case EvidenceState::Indeterminate:
      return 4;
    case EvidenceState::Unsupported:
      return 5;
    case EvidenceState::Conflicting:
      return 6;
    case EvidenceState::Expired:
      return 7;
    case EvidenceState::Refused:
      return 8;
  }
  return 8;
}

EvidenceState worse(EvidenceState left, EvidenceState right) noexcept {
  return evidence_state_rank(left) >= evidence_state_rank(right) ? left : right;
}

EntityRef EntityRef::of(EntityKind kind, std::string id) {
  EntityRef reference;
  reference.kind_ = kind;
  reference.id_ = std::move(id);
  return reference;
}

EntityRef EntityRef::feed(FeedId id) { return of(EntityKind::Feed, std::move(id).value()); }
EntityRef EntityRef::bus(BusId id) { return of(EntityKind::Bus, std::move(id).value()); }
EntityRef EntityRef::ups(UpsId id) { return of(EntityKind::Ups, std::move(id).value()); }
EntityRef EntityRef::generator(GeneratorId id) { return of(EntityKind::Generator, std::move(id).value()); }
EntityRef EntityRef::pdu(PduId id) { return of(EntityKind::Pdu, std::move(id).value()); }
EntityRef EntityRef::circuit(CircuitId id) { return of(EntityKind::Circuit, std::move(id).value()); }
EntityRef EntityRef::load(LoadId id) { return of(EntityKind::Load, std::move(id).value()); }
EntityRef EntityRef::redundancy_group(RedundancyGroupId id) {
  return of(EntityKind::RedundancyGroup, std::move(id).value());
}

std::string EntityRef::to_string() const {
  if (!valid()) {
    return "unknown:";
  }
  return std::string(po::to_string(kind_)) + ":" + id_;
}

Result<EntityRef> EntityRef::parse(std::string_view text) {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return Error(ReasonCode::ParseError, "entity reference '" + std::string(text) + "' has no ':' separator");
  }
  const std::string_view kind_text = text.substr(0, separator);
  const std::string_view id_text = text.substr(separator + 1);
  if (id_text.empty()) {
    return Error(ReasonCode::ParseError, "entity reference '" + std::string(text) + "' has an empty identifier");
  }
  for (const KindName& entry : kEntityKindNames) {
    if (entry.name == kind_text) {
      return EntityRef::of(entry.kind, std::string(id_text));
    }
  }
  return Error(ReasonCode::ParseError,
               "entity reference '" + std::string(text) + "' has an unrecognized entity kind");
}

std::string to_string(const Provenance& provenance) {
  std::string text;
  text.reserve(96);
  text.append(provenance.source.empty() ? std::string_view("<no-source>") : provenance.source.view());
  text.append(" authority=");
  text.append(po::to_string(provenance.authority));
  text.append(" origin=");
  text.append(po::to_string(provenance.origin));
  text.append(" generation=");
  text.append(std::to_string(provenance.generation.value()));
  text.append(" epoch=");
  text.append(std::to_string(provenance.epoch.value()));
  text.append(" sequence=");
  text.append(std::to_string(provenance.sequence.value()));
  return text;
}

Provenance make_observed_provenance(SourceId source, Generation generation, Epoch epoch, Sequence sequence,
                                    std::optional<Timestamp> source_time, AuthorityKind authority,
                                    Timestamp received, MonotonicInstant received_steady) {
  Provenance provenance;
  provenance.source = std::move(source);
  provenance.authority = authority;
  provenance.origin = EvidenceOrigin::LiveIngest;
  provenance.generation = generation;
  provenance.epoch = epoch;
  provenance.sequence = sequence;
  provenance.source_time = source_time;
  provenance.received_time = received;
  provenance.received_steady = received_steady;
  provenance.has_monotonic_anchor = true;
  return provenance;
}

Provenance make_recovered_provenance(SourceId source, AuthorityKind authority, Generation generation, Epoch epoch,
                                     Sequence sequence, std::optional<Timestamp> source_time,
                                     Timestamp received_time) {
  Provenance provenance;
  provenance.source = std::move(source);
  provenance.authority = authority;
  provenance.origin = EvidenceOrigin::RecoveredFromStore;
  provenance.generation = generation;
  provenance.epoch = epoch;
  provenance.sequence = sequence;
  provenance.source_time = source_time;
  provenance.received_time = received_time;
  provenance.received_steady = MonotonicInstant{};
  provenance.has_monotonic_anchor = false;
  return provenance;
}

MeasurementKind kind_of(const MeasurementValue& value) noexcept {
  return std::visit(
      [](const auto& typed) -> MeasurementKind {
        using T = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<T, Power>) {
          return MeasurementKind::ActivePower;
        } else if constexpr (std::is_same_v<T, ApparentPower>) {
          return MeasurementKind::ApparentPower;
        } else if constexpr (std::is_same_v<T, ReactivePower>) {
          return MeasurementKind::ReactivePower;
        } else if constexpr (std::is_same_v<T, Voltage>) {
          return MeasurementKind::Voltage;
        } else if constexpr (std::is_same_v<T, Current>) {
          return MeasurementKind::Current;
        } else if constexpr (std::is_same_v<T, Frequency>) {
          return MeasurementKind::Frequency;
        } else if constexpr (std::is_same_v<T, Ratio>) {
          return MeasurementKind::PowerFactor;
        } else if constexpr (std::is_same_v<T, Energy>) {
          return MeasurementKind::Energy;
        } else {
          return MeasurementKind::Temperature;
        }
      },
      value);
}

std::string value_string(const MeasurementValue& value) {
  return std::visit([](const auto& typed) { return typed.to_string(); }, value);
}

QuantityRep raw_value(const MeasurementValue& value) noexcept {
  return std::visit([](const auto& typed) { return typed.raw(); }, value);
}

EvidenceRef make_ref(const Measurement& measurement) {
  EvidenceRef reference;
  reference.source = measurement.provenance.source;
  reference.entity = measurement.entity;
  reference.kind = measurement.kind();
  reference.phase = measurement.phase;
  reference.measurement = measurement.id;
  reference.generation = measurement.provenance.generation;
  reference.received_time = measurement.provenance.received_time;
  return reference;
}

std::string to_string(const EvidenceRef& reference) {
  std::string text = reference.entity.to_string();
  text.append("/");
  text.append(po::to_string(reference.kind));
  text.append("/");
  text.append(po::to_string(reference.phase));
  text.append("@");
  text.append(reference.source.empty() ? std::string("<no-source>") : reference.source.value());
  text.append("#g");
  text.append(std::to_string(reference.generation.value()));
  text.append("#m");
  text.append(std::to_string(reference.measurement.value()));
  return text;
}

namespace {

// Grouping key for duplicate detection. A duplicate is the same measurement
// slot emitted twice under the same sequence by the same source; two different
// slots in one batch share a sequence and are not duplicates of each other.
[[nodiscard]] auto arrival_key(const Measurement& measurement) {
  return std::make_tuple(measurement.provenance.source, measurement.entity, measurement.kind(), measurement.phase,
                         measurement.provenance.sequence, measurement.id);
}

[[nodiscard]] bool arrival_less(const Measurement& left, const Measurement& right) {
  return arrival_key(left) < arrival_key(right);
}

// Canonical publication order.
[[nodiscard]] auto canonical_key(const Measurement& measurement) {
  return std::make_tuple(measurement.entity, measurement.kind(), measurement.phase, measurement.provenance.source,
                         measurement.provenance.generation, measurement.provenance.sequence, measurement.id);
}

[[nodiscard]] bool canonical_less(const Measurement& left, const Measurement& right) {
  return canonical_key(left) < canonical_key(right);
}

struct Key {
  EntityRef entity;
  MeasurementKind kind{MeasurementKind::ActivePower};
  Phase phase{Phase::Total};

  friend bool operator==(const Key&, const Key&) noexcept = default;
  friend auto operator<=>(const Key&, const Key&) noexcept = default;
};

}  // namespace

Result<EvidenceSet> EvidenceSet::build(std::vector<Measurement> measurements) {
  EvidenceSet set;
  set.admitted_count_ = measurements.size();

  for (const Measurement& measurement : measurements) {
    if (measurement.id.value() == 0) {
      return Error(ReasonCode::InvalidArgument, "measurement with entity " + measurement.entity.to_string() +
                                                    " has no measurement identifier");
    }
    if (!measurement.entity.valid()) {
      return Error(ReasonCode::InvalidArgument, "measurement " + std::to_string(measurement.id.value()) +
                                                    " has no entity reference");
    }
  }

  std::sort(measurements.begin(), measurements.end(), arrival_less);

  // Stage 1: collapse duplicate (source, sequence) pairs. A source that emits
  // the same sequence twice is contradicting itself, so the extra copies are
  // dropped and the contradiction is recorded.
  std::vector<Measurement> deduplicated;
  deduplicated.reserve(measurements.size());
  for (std::size_t index = 0; index < measurements.size(); ++index) {
    const Measurement& current = measurements[index];
    if (!deduplicated.empty()) {
      const Measurement& previous = deduplicated.back();
      if (previous.provenance.source == current.provenance.source &&
          previous.provenance.sequence == current.provenance.sequence && previous.entity == current.entity &&
          previous.kind() == current.kind() && previous.phase == current.phase) {
        EvidenceNote note;
        note.code = ReasonCode::DuplicateSample;
        note.severity = severity(note.code);
        note.detail = "source " + previous.provenance.source.value() + " emitted sequence " +
                      std::to_string(current.provenance.sequence.value()) + " more than once; " +
                      std::to_string(current.id.value()) + " was dropped in favour of " +
                      std::to_string(previous.id.value());
        note.entities.push_back(current.entity);
        set.notes_.push_back(std::move(note));
        continue;
      }
    }
    deduplicated.push_back(current);
  }

  // Stage 2: for each (entity, kind, phase, source) keep only the newest
  // generation and the highest sequence. Superseded samples are dropped so that
  // an old generation can never be silently mixed with a new one.
  std::map<std::tuple<EntityRef, MeasurementKind, Phase, SourceId>, std::size_t> newest;
  std::vector<bool> superseded(deduplicated.size(), false);
  for (std::size_t index = 0; index < deduplicated.size(); ++index) {
    const Measurement& measurement = deduplicated[index];
    const auto key = std::make_tuple(measurement.entity, measurement.kind(), measurement.phase,
                                     measurement.provenance.source);
    const auto found = newest.find(key);
    if (found == newest.end()) {
      newest.emplace(key, index);
      continue;
    }
    const Measurement& incumbent = deduplicated[found->second];
    const bool challenger_newer =
        std::tie(measurement.provenance.generation, measurement.provenance.sequence, measurement.id) >
        std::tie(incumbent.provenance.generation, incumbent.provenance.sequence, incumbent.id);
    const std::size_t loser = challenger_newer ? found->second : index;
    const std::size_t winner = challenger_newer ? index : found->second;
    const Measurement& loser_measurement = deduplicated[loser];
    const Measurement& winner_measurement = deduplicated[winner];

    EvidenceNote note;
    note.code = loser_measurement.provenance.generation == winner_measurement.provenance.generation
                    ? ReasonCode::ReorderedSample
                    : ReasonCode::GenerationMismatch;
    note.severity = severity(note.code);
    note.detail = "source " + winner_measurement.provenance.source.value() + " superseded measurement " +
                  std::to_string(loser_measurement.id.value()) + " (generation " +
                  std::to_string(loser_measurement.provenance.generation.value()) + ", sequence " +
                  std::to_string(loser_measurement.provenance.sequence.value()) + ") with measurement " +
                  std::to_string(winner_measurement.id.value()) + " (generation " +
                  std::to_string(winner_measurement.provenance.generation.value()) + ", sequence " +
                  std::to_string(winner_measurement.provenance.sequence.value()) + ")";
    note.entities.push_back(winner_measurement.entity);
    set.notes_.push_back(std::move(note));

    superseded[loser] = true;
    newest[key] = winner;
  }

  for (std::size_t index = 0; index < deduplicated.size(); ++index) {
    if (!superseded[index]) {
      set.canonical_.push_back(deduplicated[index]);
    }
  }
  set.dropped_count_ = set.admitted_count_ - set.canonical_.size();

  std::sort(set.canonical_.begin(), set.canonical_.end(), canonical_less);

  for (std::size_t index = 0; index < set.canonical_.size(); ++index) {
    const Measurement& measurement = set.canonical_[index];
    set.by_entity_[measurement.entity].push_back(index);
    const auto found = set.source_generations_.find(measurement.provenance.source);
    if (found == set.source_generations_.end() || found->second < measurement.provenance.generation) {
      set.source_generations_[measurement.provenance.source] = measurement.provenance.generation;
    }
  }

  Fnv1a64 hasher;
  for (const Measurement& measurement : set.canonical_) {
    hasher.update_separator('E');
    hasher.update(measurement.entity.to_string());
    hasher.update_separator('K');
    hasher.update_integral(static_cast<std::uint8_t>(measurement.kind()));
    hasher.update_separator('P');
    hasher.update_integral(static_cast<std::uint8_t>(measurement.phase));
    hasher.update_separator('V');
    hasher.update_integral(raw_value(measurement.value));
    hasher.update_separator('S');
    hasher.update(measurement.provenance.source.view());
    hasher.update_separator('g');
    hasher.update_integral(measurement.provenance.generation.value());
    hasher.update_separator('q');
    hasher.update_integral(measurement.provenance.sequence.value());
    hasher.update_separator('a');
    hasher.update_integral(static_cast<std::uint8_t>(measurement.provenance.authority));
    hasher.update_separator('o');
    hasher.update_integral(static_cast<std::uint8_t>(measurement.provenance.origin));
    hasher.update_separator('t');
    hasher.update_integral(measurement.provenance.received_time.unix_nanos());
  }
  set.content_hash_ = hasher.value();
  return set;
}

std::vector<const Measurement*> EvidenceSet::for_entity(const EntityRef& entity) const {
  std::vector<const Measurement*> result;
  const auto found = by_entity_.find(entity);
  if (found == by_entity_.end()) {
    return result;
  }
  result.reserve(found->second.size());
  for (const std::size_t index : found->second) {
    result.push_back(&canonical_[index]);
  }
  return result;
}

std::vector<const Measurement*> EvidenceSet::for_entity(const EntityRef& entity, MeasurementKind kind) const {
  std::vector<const Measurement*> result;
  for (const Measurement* measurement : for_entity(entity)) {
    if (measurement->kind() == kind) {
      result.push_back(measurement);
    }
  }
  return result;
}

const Measurement* EvidenceSet::find(MeasurementId id) const {
  for (const Measurement& measurement : canonical_) {
    if (measurement.id == id) {
      return &measurement;
    }
  }
  return nullptr;
}

const Measurement* EvidenceSet::latest(const EntityRef& entity, MeasurementKind kind, Phase phase) const {
  const Measurement* best = nullptr;
  for (const Measurement* measurement : for_entity(entity)) {
    if (measurement->kind() != kind || measurement->phase != phase) {
      continue;
    }
    if (best == nullptr ||
        std::tie(measurement->provenance.generation, measurement->provenance.sequence, measurement->id) >
            std::tie(best->provenance.generation, best->provenance.sequence, best->id)) {
      best = measurement;
    }
  }
  return best;
}

std::vector<EvidenceNote> EvidenceSet::conflicting(QuantityRep absolute_tolerance) const {
  std::vector<EvidenceNote> notes;
  std::map<std::tuple<EntityRef, MeasurementKind, Phase>, std::vector<const Measurement*>> groups;
  for (const Measurement& measurement : canonical_) {
    groups[std::make_tuple(measurement.entity, measurement.kind(), measurement.phase)].push_back(&measurement);
  }
  for (const auto& entry : groups) {
    const std::vector<const Measurement*>& members = entry.second;
    for (std::size_t left = 0; left < members.size(); ++left) {
      for (std::size_t right = left + 1; right < members.size(); ++right) {
        if (members[left]->provenance.source == members[right]->provenance.source) {
          continue;
        }
        const QuantityRep left_value = raw_value(members[left]->value);
        const QuantityRep right_value = raw_value(members[right]->value);
        QuantityRep difference{};
        if (!checked_sub(left_value, right_value, difference)) {
          difference = std::numeric_limits<QuantityRep>::max();
        }
        const std::uint64_t magnitude = detail::magnitude(difference);
        const std::uint64_t tolerance =
            absolute_tolerance < 0 ? 0ull : static_cast<std::uint64_t>(absolute_tolerance);
        if (magnitude <= tolerance) {
          continue;
        }
        EvidenceNote note;
        note.code = ReasonCode::ConflictingMeters;
        note.severity = severity(note.code);
        note.detail = members[left]->entity.to_string() + " " + std::string(po::to_string(members[left]->kind())) +
                      " " + std::string(po::to_string(members[left]->phase)) + ": source " +
                      members[left]->provenance.source.value() + " reports " +
                      value_string(members[left]->value) + " while source " +
                      members[right]->provenance.source.value() + " reports " +
                      value_string(members[right]->value);
        note.entities.push_back(members[left]->entity);
        notes.push_back(std::move(note));
      }
    }
  }
  return notes;
}

std::vector<Generation> EvidenceSet::generations() const {
  std::vector<Generation> result;
  result.reserve(source_generations_.size());
  for (const auto& entry : source_generations_) {
    result.push_back(entry.second);
  }
  return result;
}

Generation EvidenceSet::generation_of(const SourceId& source) const {
  const auto found = source_generations_.find(source);
  return found == source_generations_.end() ? Generation{} : found->second;
}

std::vector<SourceId> EvidenceSet::sources() const {
  std::vector<SourceId> result;
  result.reserve(source_generations_.size());
  for (const auto& entry : source_generations_) {
    result.push_back(entry.first);
  }
  return result;
}

std::vector<EntityRef> EvidenceSet::entities() const {
  std::vector<EntityRef> result;
  result.reserve(by_entity_.size());
  for (const auto& entry : by_entity_) {
    result.push_back(entry.first);
  }
  return result;
}

std::uint64_t EvidenceSet::content_hash() const noexcept { return content_hash_; }

bool is_power_measurement(MeasurementKind kind) noexcept { return is_power_kind(kind); }

}  // namespace po
