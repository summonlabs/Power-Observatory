// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "power_observatory/quantity.hpp"
#include "power_observatory/reason.hpp"
#include "power_observatory/result.hpp"
#include "power_observatory/strong.hpp"
#include "power_observatory/time.hpp"

namespace po {

// ---------------------------------------------------------------------------
// Epistemic vocabulary
// ---------------------------------------------------------------------------

// How a statement came to be known. This is deliberately separate from how
// recent it is: an authority can be current and still not be an observation of
// electrical reality.
enum class AuthorityKind : std::uint8_t {
  Unknown = 0,
  // Measured by a metering device attached to the plant.
  Observed = 1,
  // Computed by this runtime from other evidence. The derivation is always
  // recorded alongside the value.
  Derived = 2,
  // Declared by a human or a configuration system. Configuration describes
  // intent, never measured state.
  Configured = 3,
  // A controller reports that it accepted a request. Acknowledgement is not
  // evidence that the requested electrical effect occurred.
  Acknowledged = 4,
  // Asserted by an adjacent authority through its published contract. It is
  // consumed as evidence with that authority named; it never transfers the
  // adjacent authority's decision rights to this runtime.
  External = 5,
  // Produced by a scenario generator or recorded fixture. Never presented as
  // plant evidence.
  Synthetic = 6,
};

// Where the bytes currently in hand came from. Orthogonal to AuthorityKind:
// a recovered record still remembers the authority that originally produced it.
enum class EvidenceOrigin : std::uint8_t {
  LiveIngest = 0,
  RecoveredFromStore = 1,
  SyntheticScenario = 2,
};

enum class EntityKind : std::uint8_t {
  Unknown = 0,
  Feed = 1,
  Bus = 2,
  Ups = 3,
  Generator = 4,
  Pdu = 5,
  Circuit = 6,
  Load = 7,
  RedundancyGroup = 8,
};

enum class MeasurementKind : std::uint8_t {
  ActivePower = 0,
  ApparentPower = 1,
  ReactivePower = 2,
  Voltage = 3,
  Current = 4,
  Frequency = 5,
  PowerFactor = 6,
  Energy = 7,
  Temperature = 8,
};

enum class Phase : std::uint8_t {
  Total = 0,
  A = 1,
  B = 2,
  C = 3,
  AB = 4,
  BC = 5,
  CA = 6,
  Neutral = 7,
  Unknown = 8,
};

enum class FreshnessClass : std::uint8_t {
  Unknown = 0,
  // Observed by this process, inside the fresh window.
  Fresh = 1,
  // Recent by the clock, but not observed by this process: it was read back
  // from durable storage. Usable, and never the current state of the plant.
  Recovered = 2,
  // Past the fresh window but inside the aging window.
  Aging = 3,
  Stale = 4,
  Expired = 5,
  NotApplicable = 6,
};

// The determination attached to every answer this runtime produces. Absence of
// a value is always one of these states, never an empty default.
enum class EvidenceState : std::uint8_t {
  Unknown = 0,
  // Established from evidence this process observed.
  Known = 1,
  // Established from evidence that is recent but was read back from durable
  // storage. Explicitly not the current state of the plant.
  Recovered = 2,
  Stale = 3,
  Conflicting = 4,
  Indeterminate = 5,
  Unsupported = 6,
  Refused = 7,
  Expired = 8,
};

[[nodiscard]] std::string_view to_string(AuthorityKind kind) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceOrigin origin) noexcept;
[[nodiscard]] std::string_view to_string(EntityKind kind) noexcept;
[[nodiscard]] std::string_view to_string(MeasurementKind kind) noexcept;
[[nodiscard]] std::string_view to_string(Phase phase) noexcept;
[[nodiscard]] std::string_view to_string(FreshnessClass classification) noexcept;
[[nodiscard]] std::string_view to_string(EvidenceState state) noexcept;

// Authority kinds whose payload is a measurement of electrical state.
[[nodiscard]] constexpr bool carries_measurement(AuthorityKind kind) noexcept {
  return kind == AuthorityKind::Observed || kind == AuthorityKind::Derived || kind == AuthorityKind::External;
}

// Authority kinds that may be used as the sole basis for a claim about what the
// electrical plant is physically doing right now.
[[nodiscard]] constexpr bool is_first_hand_observation(AuthorityKind kind) noexcept {
  return kind == AuthorityKind::Observed;
}

[[nodiscard]] constexpr bool is_configured(AuthorityKind kind) noexcept {
  return kind == AuthorityKind::Configured;
}

[[nodiscard]] constexpr bool is_acknowledgement(AuthorityKind kind) noexcept {
  return kind == AuthorityKind::Acknowledged;
}

// Maps a temporal classification onto the determination vocabulary.
[[nodiscard]] EvidenceState to_evidence_state(FreshnessClass classification) noexcept;

// Ordering of determinations from most to least certain. Used to aggregate the
// state of a composite answer without ever upgrading an uncertain part.
[[nodiscard]] int evidence_state_rank(EvidenceState state) noexcept;
[[nodiscard]] EvidenceState worse(EvidenceState left, EvidenceState right) noexcept;

// ---------------------------------------------------------------------------
// Entities
// ---------------------------------------------------------------------------

// A typed, printable reference to one plant entity. Parsing is strict so that a
// typo becomes a refusal instead of a silently different entity.
class EntityRef {
 public:
  EntityRef() = default;

  [[nodiscard]] static EntityRef of(EntityKind kind, std::string id);

  [[nodiscard]] static EntityRef feed(FeedId id);
  [[nodiscard]] static EntityRef bus(BusId id);
  [[nodiscard]] static EntityRef ups(UpsId id);
  [[nodiscard]] static EntityRef generator(GeneratorId id);
  [[nodiscard]] static EntityRef pdu(PduId id);
  [[nodiscard]] static EntityRef circuit(CircuitId id);
  [[nodiscard]] static EntityRef load(LoadId id);
  [[nodiscard]] static EntityRef redundancy_group(RedundancyGroupId id);

  [[nodiscard]] EntityKind kind() const noexcept { return kind_; }
  [[nodiscard]] const std::string& id() const noexcept { return id_; }
  [[nodiscard]] bool valid() const noexcept { return kind_ != EntityKind::Unknown && !id_.empty(); }

  // "feed:main-a"
  [[nodiscard]] std::string to_string() const;
  [[nodiscard]] static Result<EntityRef> parse(std::string_view text);

  friend bool operator==(const EntityRef&, const EntityRef&) noexcept = default;
  friend auto operator<=>(const EntityRef&, const EntityRef&) noexcept = default;

 private:
  EntityKind kind_{EntityKind::Unknown};
  std::string id_;
};

// ---------------------------------------------------------------------------
// Provenance
// ---------------------------------------------------------------------------

struct Provenance {
  SourceId source;
  AuthorityKind authority{AuthorityKind::Unknown};
  EvidenceOrigin origin{EvidenceOrigin::LiveIngest};
  Generation generation{};
  Epoch epoch{};
  Sequence sequence{};
  // What the producing source claims the measurement instant was. A source can
  // lie, and its clock can drift, so this is recorded rather than trusted.
  std::optional<Timestamp> source_time;
  // When this process took delivery of the evidence.
  Timestamp received_time{};
  // Monotonic anchor for the delivery. It is meaningful only inside the
  // process that observed it, so it is never written to durable storage.
  MonotonicInstant received_steady{};
  bool has_monotonic_anchor{false};

  [[nodiscard]] bool recovered() const noexcept {
    return origin == EvidenceOrigin::RecoveredFromStore;
  }

  // Age can only be measured on a monotonic basis inside the observing
  // process. Recovered evidence therefore can never be classified as fresh,
  // however recent its wall-clock timestamps look.
  [[nodiscard]] bool can_measure_age_monotonically() const noexcept {
    return has_monotonic_anchor && !recovered();
  }
};

[[nodiscard]] std::string to_string(const Provenance& provenance);

// Builds provenance for evidence observed by this process right now.
[[nodiscard]] Provenance make_observed_provenance(SourceId source, Generation generation, Epoch epoch,
                                                  Sequence sequence, std::optional<Timestamp> source_time,
                                                  AuthorityKind authority, Timestamp received,
                                                  MonotonicInstant received_steady);

// Builds provenance for evidence read back from durable storage. The original
// source and authority are preserved; the monotonic anchor is deliberately
// absent and the origin marks the record as recovered.
[[nodiscard]] Provenance make_recovered_provenance(SourceId source, AuthorityKind authority,
                                                   Generation generation, Epoch epoch, Sequence sequence,
                                                   std::optional<Timestamp> source_time,
                                                   Timestamp received_time);

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

// The value domain of a measurement. Using a variant rather than a bare integer
// makes it impossible to compare watts with volts by accident.
using MeasurementValue =
    std::variant<Power, ApparentPower, ReactivePower, Voltage, Current, Frequency, Ratio, Energy, Temperature>;

[[nodiscard]] MeasurementKind kind_of(const MeasurementValue& value) noexcept;
// True for measurement kinds carried as a power-like quantity.
[[nodiscard]] bool is_power_measurement(MeasurementKind kind) noexcept;
[[nodiscard]] std::string value_string(const MeasurementValue& value);
[[nodiscard]] QuantityRep raw_value(const MeasurementValue& value) noexcept;

struct Measurement {
  MeasurementId id{};
  EntityRef entity;
  Phase phase{Phase::Total};
  MeasurementValue value{Power{}};
  Provenance provenance;

  [[nodiscard]] MeasurementKind kind() const noexcept { return kind_of(value); }
  [[nodiscard]] bool valid() const noexcept { return id.value() != 0 && entity.valid(); }
};

// A durable, orderable handle to the exact evidence behind a conclusion.
// Members are declared in comparison order.
struct EvidenceRef {
  SourceId source;
  EntityRef entity;
  MeasurementKind kind{MeasurementKind::ActivePower};
  Phase phase{Phase::Total};
  MeasurementId measurement{};
  Generation generation{};
  Timestamp received_time{};

  friend bool operator==(const EvidenceRef&, const EvidenceRef&) noexcept = default;
  friend auto operator<=>(const EvidenceRef&, const EvidenceRef&) noexcept = default;
};

[[nodiscard]] EvidenceRef make_ref(const Measurement& measurement);
[[nodiscard]] std::string to_string(const EvidenceRef& reference);

// ---------------------------------------------------------------------------
// Evidence set
// ---------------------------------------------------------------------------

// A non-fatal observation about the evidence itself: duplicates, reordering,
// superseded generations, sources that disagree.
struct EvidenceNote {
  ReasonCode code{ReasonCode::None};
  Severity severity{Severity::Info};
  std::string detail;
  std::vector<EntityRef> entities;
};

// An immutable, canonically ordered collection of evidence.
//
// Construction is the only place where duplicates, reordering, and generation
// supersession are resolved. After construction the set is deterministic: two
// inputs that differ only in arrival order produce byte-identical sets.
class EvidenceSet {
 public:
  EvidenceSet() = default;

  // Admits measurements, resolving duplicates, reordering, and generation
  // supersession. Never fails on contradictory content; contradictions are
  // recorded as notes and remain visible through conflicting().
  [[nodiscard]] static Result<EvidenceSet> build(std::vector<Measurement> measurements);

  [[nodiscard]] std::size_t size() const noexcept { return canonical_.size(); }
  [[nodiscard]] std::size_t admitted_count() const noexcept { return admitted_count_; }
  [[nodiscard]] std::size_t dropped_count() const noexcept { return dropped_count_; }
  [[nodiscard]] bool empty() const noexcept { return canonical_.empty(); }

  [[nodiscard]] const std::vector<Measurement>& measurements() const noexcept { return canonical_; }
  [[nodiscard]] const std::vector<EvidenceNote>& notes() const noexcept { return notes_; }

  [[nodiscard]] std::vector<const Measurement*> for_entity(const EntityRef& entity) const;
  [[nodiscard]] std::vector<const Measurement*> for_entity(const EntityRef& entity, MeasurementKind kind) const;
  [[nodiscard]] const Measurement* find(MeasurementId id) const;

  // The measurement with the highest (generation, sequence, measurement id) for
  // the given key, or nullptr when nothing was admitted.
  [[nodiscard]] const Measurement* latest(const EntityRef& entity, MeasurementKind kind, Phase phase) const;

  // Every measurement sharing an entity/kind/phase key that came from more than
  // one source with different values beyond the given tolerance.
  [[nodiscard]] std::vector<EvidenceNote> conflicting(QuantityRep absolute_tolerance) const;

  [[nodiscard]] std::vector<Generation> generations() const;
  [[nodiscard]] Generation generation_of(const SourceId& source) const;
  [[nodiscard]] std::vector<SourceId> sources() const;
  [[nodiscard]] std::vector<EntityRef> entities() const;

  // Deterministic fingerprint over the canonical content.
  [[nodiscard]] std::uint64_t content_hash() const noexcept;

 private:
  std::vector<Measurement> canonical_;
  std::map<EntityRef, std::vector<std::size_t>> by_entity_;
  std::map<SourceId, Generation> source_generations_;
  std::vector<EvidenceNote> notes_;
  std::size_t admitted_count_{0};
  std::size_t dropped_count_{0};
  std::uint64_t content_hash_{0};
};

}  // namespace po
