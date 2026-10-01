// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "power_observatory/attribution.hpp"
#include "power_observatory/context.hpp"
#include "power_observatory/failover.hpp"
#include "power_observatory/flow.hpp"
#include "power_observatory/quality.hpp"
#include "power_observatory/reserve.hpp"

namespace po {

struct SnapshotMetadata {
  Revision revision{};
  Epoch epoch{};
  Timestamp computed_at{};
  MonotonicInstant computed_steady{};
  std::size_t measurement_count{0};
  std::size_t dropped_measurement_count{0};
  std::vector<Generation> generations;
  std::uint64_t evidence_hash{0};
  std::uint64_t topology_hash{0};
  std::uint64_t policy_hash{0};
};

// One answer to the core question, assembled from the engines.
//
// Every optional field is optional on purpose: an absent value means the
// question could not be answered from the evidence in hand, which is materially
// different from an answer of zero.
struct AnswerReport {
  EvidenceState state{EvidenceState::Unknown};
  Revision revision{};
  Timestamp computed_at{};
  std::optional<Power> total_observed_load;
  std::optional<Power> total_usable_capacity;
  std::optional<Power> total_reserve;
  std::size_t measured_entities{0};
  std::size_t unmeasured_entities{0};
  std::size_t unattributed_imbalances{0};
  std::size_t quality_findings{0};
  std::size_t failover_ready{0};
  std::size_t failover_degraded{0};
  std::size_t failover_not_ready{0};
  std::size_t failover_unknown{0};
  std::vector<FailoverReport> groups;
  Explanation explanation;

  [[nodiscard]] std::string to_text() const;
};

// An immutable, fully computed view of one evidence generation.
//
// A snapshot is built once and never mutated afterwards, so any number of
// reader threads may query it concurrently without synchronisation. A new
// snapshot is published by constructing a new instance and swapping a shared
// pointer, never by mutating an existing one.
class Snapshot {
 public:
  Snapshot() = default;

  [[nodiscard]] static Result<Snapshot> build(Revision revision, EvidenceSet evidence, TopologyModel topology,
                                              ObservationPolicy policy, Timestamp computed_at,
                                              MonotonicInstant computed_steady);

  [[nodiscard]] const SnapshotMetadata& metadata() const noexcept { return metadata_; }
  [[nodiscard]] const EvidenceSet& evidence() const noexcept { return evidence_; }
  [[nodiscard]] const TopologyModel& topology() const noexcept { return topology_; }
  [[nodiscard]] const ObservationPolicy& policy() const noexcept { return policy_; }
  [[nodiscard]] const FreshnessModel& freshness() const noexcept { return freshness_; }

  [[nodiscard]] ObservationContext context() const noexcept {
    return ObservationContext{evidence_, topology_, freshness_, policy_, metadata_.computed_at,
                              metadata_.computed_steady};
  }

  [[nodiscard]] Outcome<FlowReport> flow(const FlowQuery& query) const;
  [[nodiscard]] Outcome<ReserveReport> reserve(const ReserveQuery& query) const;
  [[nodiscard]] Outcome<AttributionReport> attribution(const AttributionQuery& query) const;
  [[nodiscard]] Outcome<QualityReport> quality(const QualityQuery& query) const;
  [[nodiscard]] Outcome<FailoverReport> failover(const FailoverQuery& query) const;

  // The composed core question: where is power flowing, how much usable reserve
  // remains, where are imbalance, loss and quality constraints emerging, how
  // ready is failover, and which evidence supports each conclusion.
  [[nodiscard]] AnswerReport answer() const;

 private:
  SnapshotMetadata metadata_{};
  EvidenceSet evidence_{};
  TopologyModel topology_{};
  ObservationPolicy policy_{};
  FreshnessModel freshness_{};
};

using SnapshotHandle = std::shared_ptr<const Snapshot>;

}  // namespace po
