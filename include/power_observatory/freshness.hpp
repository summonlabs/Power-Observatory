// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <optional>
#include <string>

#include "power_observatory/evidence.hpp"
#include "power_observatory/policy.hpp"
#include "power_observatory/quantity.hpp"

namespace po {

// How the age of a piece of evidence was established.
enum class FreshnessBasis : std::uint8_t {
  None = 0,
  // Measured against a monotonic clock inside the observing process. Immune to
  // wall-clock steps, and unavailable to recovered evidence.
  Monotonic = 1,
  // Approximated from wall-clock timestamps. Recorded, but never allowed to
  // promote recovered evidence to fresh.
  WallClock = 2,
};

[[nodiscard]] std::string_view to_string(FreshnessBasis basis) noexcept;

struct FreshnessAssessment {
  FreshnessClass classification{FreshnessClass::Unknown};
  FreshnessBasis basis{FreshnessBasis::None};
  std::optional<Duration> age;
  FreshnessBudget budget{};
  ReasonCode reason{ReasonCode::FreshnessUnknown};
  std::string detail;
  // A second finding that is orthogonal to the age band: a source clock that
  // disagrees with local arrival, or a wall clock that moved backwards. Callers
  // publish it alongside the band rather than folding one into the other.
  std::optional<ReasonCode> anomaly;
  std::string anomaly_detail;

  // Usable evidence may inform an answer, but is always reported with its
  // freshness attached. Recovered evidence is usable: it is recent, and it is
  // the best this process has.
  [[nodiscard]] bool usable() const noexcept {
    return classification == FreshnessClass::Fresh || classification == FreshnessClass::Recovered ||
           classification == FreshnessClass::Aging;
  }

  // Only evidence this process observed may be presented as the current state
  // of the plant. Recovered evidence never is, however recent it is.
  [[nodiscard]] bool current() const noexcept { return classification == FreshnessClass::Fresh; }
};

// The single place where age is decided. Ordering from most to least
// trustworthy: Fresh, Aging, Stale, NotApplicable, Unknown, Expired.
class FreshnessModel {
 public:
  FreshnessModel() = default;
  explicit FreshnessModel(FreshnessPolicy policy) : policy_(std::move(policy)) {}

  [[nodiscard]] const FreshnessPolicy& policy() const noexcept { return policy_; }

  [[nodiscard]] FreshnessAssessment assess(const Provenance& provenance, Timestamp wall_now,
                                           MonotonicInstant steady_now) const;
  [[nodiscard]] FreshnessAssessment assess(const Measurement& measurement, Timestamp wall_now,
                                           MonotonicInstant steady_now) const;
  // The least trustworthy classification over every measurement in the set.
  [[nodiscard]] FreshnessAssessment assess(const EvidenceSet& evidence, Timestamp wall_now,
                                           MonotonicInstant steady_now) const;

  [[nodiscard]] static FreshnessClass worse(FreshnessClass left, FreshnessClass right) noexcept;
  [[nodiscard]] static int rank(FreshnessClass classification) noexcept;

 private:
  FreshnessPolicy policy_;
};

}  // namespace po
