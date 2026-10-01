// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/freshness.hpp"

#include <limits>
#include <string>

namespace po {
namespace {

struct Band {
  FreshnessClass classification{FreshnessClass::Unknown};
  ReasonCode reason{ReasonCode::FreshnessUnknown};
  std::string detail;
};

[[nodiscard]] Band classify(Duration age, const FreshnessBudget& budget) {
  if (age.raw() < 0) {
    return Band{FreshnessClass::Unknown, ReasonCode::ClockSkewExceeded,
                "measured age is negative, which means the clock moved backwards; no age can be trusted"};
  }
  if (age <= budget.fresh_within) {
    return Band{FreshnessClass::Fresh, ReasonCode::Ok, "within the fresh window"};
  }
  if (age <= budget.aging_within) {
    return Band{FreshnessClass::Aging, ReasonCode::AgingEvidence, "past the fresh window but within the aging window"};
  }
  if (age <= budget.stale_within) {
    return Band{FreshnessClass::Stale, ReasonCode::StaleEvidence, "past the aging window but within the stale window"};
  }
  return Band{FreshnessClass::Expired, ReasonCode::ExpiredEvidence, "past the stale window"};
}

[[nodiscard]] std::string describe_age(Duration age) {
  return age.raw() < 0 ? "negative" : to_compact_string(age);
}

}  // namespace

std::string_view to_string(FreshnessBasis basis) noexcept {
  switch (basis) {
    case FreshnessBasis::None:
      return "none";
    case FreshnessBasis::Monotonic:
      return "monotonic";
    case FreshnessBasis::WallClock:
      return "wall_clock";
  }
  return "none";
}

int FreshnessModel::rank(FreshnessClass classification) noexcept {
  switch (classification) {
    case FreshnessClass::Fresh:
      return 0;
    case FreshnessClass::Recovered:
      return 1;
    case FreshnessClass::Aging:
      return 2;
    case FreshnessClass::Stale:
      return 3;
    case FreshnessClass::NotApplicable:
      return 4;
    case FreshnessClass::Unknown:
      return 5;
    case FreshnessClass::Expired:
      return 6;
  }
  return 6;
}

FreshnessClass FreshnessModel::worse(FreshnessClass left, FreshnessClass right) noexcept {
  return rank(left) >= rank(right) ? left : right;
}

FreshnessAssessment FreshnessModel::assess(const Provenance& provenance, Timestamp wall_now,
                                           MonotonicInstant steady_now) const {
  FreshnessAssessment assessment;
  const FreshnessBudget& budget = policy_.budget_for(provenance.source);
  assessment.budget = budget;

  if (provenance.source.empty()) {
    assessment.reason = ReasonCode::UnknownSource;
    assessment.detail = "evidence carries no source identifier, so no freshness budget can be applied";
    return assessment;
  }

  const std::string origin = "source " + provenance.source.value();

  if (provenance.authority == AuthorityKind::Unknown) {
    assessment.reason = ReasonCode::FreshnessUnknown;
    assessment.detail = origin + " has unknown authority, so its age cannot be interpreted";
    return assessment;
  }

  if (is_acknowledgement(provenance.authority)) {
    assessment.classification = FreshnessClass::NotApplicable;
    assessment.reason = ReasonCode::AcknowledgementNotEffect;
    assessment.detail = origin + " reports an acknowledgement, which is not a measurement of the plant";
    return assessment;
  }

  // Source-clock agreement is evaluated independently of age: the age uses the
  // local monotonic clock and is unaffected by the source's own clock.
  if (provenance.source_time.has_value()) {
    const Result<Duration> skew = sub(provenance.received_time, *provenance.source_time);
    if (!skew) {
      assessment.reason = ReasonCode::ValueOutOfRange;
      assessment.detail = origin + " reports a source instant that cannot be compared with local arrival";
      return assessment;
    }
    const QuantityRep skew_nanos = skew.value().raw();
    const QuantityRep allowed = policy_.max_clock_skew.raw() < 0 ? 0 : policy_.max_clock_skew.raw();
    if (skew_nanos < -allowed) {
      QuantityRep magnitude{};
      if (!checked_neg(skew_nanos, magnitude)) {
        magnitude = std::numeric_limits<QuantityRep>::max();
      }
      assessment.anomaly = ReasonCode::FutureTimestamp;
      assessment.anomaly_detail = origin + " reports an instant " + describe_age(Duration::from_raw(magnitude)) +
                                  " ahead of local arrival, beyond the allowed skew of " +
                                  to_compact_string(policy_.max_clock_skew);
    } else if (skew_nanos > allowed) {
      assessment.anomaly = ReasonCode::ClockSkewExceeded;
      assessment.anomaly_detail = origin + " reports an instant " + describe_age(skew.value()) +
                                  " behind local arrival, beyond the allowed skew of " +
                                  to_compact_string(policy_.max_clock_skew);
    }
  } else {
    assessment.anomaly = ReasonCode::SourceTimeMissing;
    assessment.anomaly_detail = origin + " supplied no measurement instant; only local arrival time is known";
  }

  if (provenance.can_measure_age_monotonically()) {
    const Result<Duration> age = sub(steady_now, provenance.received_steady);
    if (!age) {
      assessment.reason = ReasonCode::ValueOutOfRange;
      assessment.detail = origin + " has a delivery anchor that cannot be compared with the current instant";
      return assessment;
    }
    assessment.basis = FreshnessBasis::Monotonic;
    assessment.age = age.value();
    const Band band = classify(age.value(), budget);
    assessment.classification = band.classification;
    assessment.reason = band.reason;
    assessment.detail = origin + " observed locally " + describe_age(age.value()) + " ago, " + band.detail;
    return assessment;
  }

  if (provenance.recovered()) {
    // Recovered evidence carries no in-process anchor, so its age can only be
    // approximated from wall-clock timestamps. It is never fresh: the process
    // currently holding it did not observe it.
    assessment.basis = FreshnessBasis::WallClock;
    const Result<Duration> age = sub(wall_now, provenance.received_time);
    if (!age) {
      assessment.reason = ReasonCode::ValueOutOfRange;
      assessment.detail = origin + " was recorded at an instant that cannot be compared with the current instant";
      return assessment;
    }
    assessment.age = age.value();
    const Band band = classify(age.value(), budget);
    if (band.classification == FreshnessClass::Fresh) {
      // The age band says fresh, but the process holding these bytes did not
      // observe them. Recovered is a class of its own precisely so that neither
      // of those two facts has to be stretched to fit the other: the evidence
      // stays usable, and it is never reported as the current state.
      assessment.classification = FreshnessClass::Recovered;
      assessment.reason = ReasonCode::RecoveredNotFresh;
      assessment.detail = origin + " was recorded " + describe_age(age.value()) +
                          " ago and read back from durable storage; it is usable but it is not the current state "
                          "of the plant";
      return assessment;
    }
    assessment.classification = band.classification;
    assessment.reason = band.reason;
    assessment.detail = origin + " was recovered from durable storage and is " + describe_age(age.value()) +
                        " old by wall clock, " + band.detail;
    return assessment;
  }

  assessment.basis = FreshnessBasis::None;
  assessment.reason = ReasonCode::MonotonicAnchorLost;
  assessment.detail = origin + " has neither a live delivery anchor nor a recovery marker, so its age is unknown";
  return assessment;
}

FreshnessAssessment FreshnessModel::assess(const Measurement& measurement, Timestamp wall_now,
                                           MonotonicInstant steady_now) const {
  FreshnessAssessment assessment = assess(measurement.provenance, wall_now, steady_now);
  if (assessment.detail.empty()) {
    assessment.detail = measurement.entity.to_string();
  } else {
    assessment.detail = measurement.entity.to_string() + " " +
                        std::string(po::to_string(measurement.kind())) + ": " + assessment.detail;
  }
  return assessment;
}

FreshnessAssessment FreshnessModel::assess(const EvidenceSet& evidence, Timestamp wall_now,
                                           MonotonicInstant steady_now) const {
  FreshnessAssessment aggregate;
  if (evidence.empty()) {
    aggregate.reason = ReasonCode::NoEvidence;
    aggregate.detail = "the evidence set is empty";
    return aggregate;
  }

  bool first = true;
  for (const Measurement& measurement : evidence.measurements()) {
    const FreshnessAssessment current = assess(measurement, wall_now, steady_now);
    if (first) {
      aggregate = current;
      first = false;
      continue;
    }
    if (rank(current.classification) > rank(aggregate.classification)) {
      const std::string previous_detail = aggregate.detail;
      aggregate = current;
      aggregate.detail = current.detail + " (least trustworthy of " +
                         std::to_string(evidence.measurements().size()) + " measurements)";
      (void)previous_detail;
    }
    if (current.anomaly.has_value() && !aggregate.anomaly.has_value()) {
      aggregate.anomaly = current.anomaly;
      aggregate.anomaly_detail = current.anomaly_detail;
    }
  }
  return aggregate;
}

}  // namespace po
