// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <string>

#include "power_observatory/freshness.hpp"

using namespace po;

namespace {

constexpr QuantityRep kSecond = 1000000000;

Provenance live_provenance(Sequence sequence = Sequence{1}) {
  return make_observed_provenance(SourceId("meter-1"), Generation{1}, Epoch{}, sequence, std::nullopt,
                                  AuthorityKind::Observed, Timestamp::from_unix_nanos(0),
                                  MonotonicInstant::from_nanos(0));
}

}  // namespace

PO_TEST(freshness, bands_are_decided_by_measured_age) {
  FreshnessModel model;
  const Provenance provenance = live_provenance();

  PO_CHECK_EQ(model.assess(provenance, Timestamp{}, MonotonicInstant::from_nanos(0)).classification,
             FreshnessClass::Fresh);
  PO_CHECK_EQ(model.assess(provenance, Timestamp{}, MonotonicInstant::from_nanos(6 * kSecond)).classification,
             FreshnessClass::Aging);
  PO_CHECK_EQ(model.assess(provenance, Timestamp{}, MonotonicInstant::from_nanos(20 * kSecond)).classification,
             FreshnessClass::Stale);
  PO_CHECK_EQ(model.assess(provenance, Timestamp{}, MonotonicInstant::from_nanos(120 * kSecond)).classification,
             FreshnessClass::Expired);
}

PO_TEST(freshness, recovered_evidence_is_never_fresh) {
  FreshnessModel model;
  const Provenance recovered = make_recovered_provenance(SourceId("meter-1"), AuthorityKind::Observed,
                                                         Generation{1}, Epoch{}, Sequence{1}, std::nullopt,
                                                         Timestamp::from_unix_nanos(0));
  // One second old by the wall clock: inside the fresh window, and still not
  // fresh, because this process did not observe it.
  const FreshnessAssessment assessment =
      model.assess(recovered, Timestamp::from_unix_nanos(1 * kSecond), MonotonicInstant::from_nanos(0));
  PO_CHECK_EQ(assessment.classification, FreshnessClass::Recovered);
  PO_CHECK_EQ(assessment.reason, ReasonCode::RecoveredNotFresh);
  PO_CHECK_EQ(assessment.basis, FreshnessBasis::WallClock);
  PO_CHECK(assessment.usable());
  PO_CHECK(!assessment.current());
}

PO_TEST(freshness, recovered_evidence_expires_by_wall_clock) {
  FreshnessModel model;
  const Provenance recovered = make_recovered_provenance(SourceId("meter-1"), AuthorityKind::Observed,
                                                         Generation{1}, Epoch{}, Sequence{1}, std::nullopt,
                                                         Timestamp::from_unix_nanos(0));
  PO_CHECK_EQ(
      model.assess(recovered, Timestamp::from_unix_nanos(20 * kSecond), MonotonicInstant::from_nanos(0)).classification,
      FreshnessClass::Stale);
  PO_CHECK_EQ(model
                  .assess(recovered, Timestamp::from_unix_nanos(120 * kSecond), MonotonicInstant::from_nanos(0))
                  .classification,
              FreshnessClass::Expired);
}

PO_TEST(freshness, an_unstamped_live_record_cannot_be_aged) {
  FreshnessModel model;
  Provenance provenance = live_provenance();
  provenance.has_monotonic_anchor = false;
  const FreshnessAssessment assessment =
      model.assess(provenance, Timestamp::from_unix_nanos(0), MonotonicInstant::from_nanos(0));
  PO_CHECK_EQ(assessment.classification, FreshnessClass::Unknown);
  PO_CHECK_EQ(assessment.reason, ReasonCode::MonotonicAnchorLost);
  PO_CHECK(!assessment.usable());
}

PO_TEST(freshness, acknowledgement_has_no_freshness_class) {
  FreshnessModel model;
  Provenance provenance = live_provenance();
  provenance.authority = AuthorityKind::Acknowledged;
  const FreshnessAssessment assessment =
      model.assess(provenance, Timestamp::from_unix_nanos(0), MonotonicInstant::from_nanos(0));
  PO_CHECK_EQ(assessment.classification, FreshnessClass::NotApplicable);
  PO_CHECK_EQ(assessment.reason, ReasonCode::AcknowledgementNotEffect);
  PO_CHECK(!assessment.usable());
}

PO_TEST(freshness, missing_source_is_refused_rather_than_guessed) {
  FreshnessModel model;
  Provenance provenance = live_provenance();
  provenance.source = SourceId("");
  const FreshnessAssessment assessment =
      model.assess(provenance, Timestamp::from_unix_nanos(0), MonotonicInstant::from_nanos(0));
  PO_CHECK_EQ(assessment.classification, FreshnessClass::Unknown);
  PO_CHECK_EQ(assessment.reason, ReasonCode::UnknownSource);
}

PO_TEST(freshness, source_clock_disagreement_is_reported_but_does_not_change_the_age) {
  FreshnessModel model;
  Provenance provenance = live_provenance();
  provenance.received_time = Timestamp::from_unix_nanos(100 * kSecond);
  provenance.source_time = Timestamp::from_unix_nanos(0);
  const FreshnessAssessment assessment =
      model.assess(provenance, Timestamp::from_unix_nanos(100 * kSecond), MonotonicInstant::from_nanos(0));
  PO_CHECK_EQ(assessment.classification, FreshnessClass::Fresh);
  PO_REQUIRE(assessment.anomaly.has_value());
  PO_CHECK_EQ(*assessment.anomaly, ReasonCode::ClockSkewExceeded);
}

PO_TEST(freshness, a_source_ahead_of_local_arrival_is_a_future_timestamp) {
  FreshnessModel model;
  Provenance provenance = live_provenance();
  provenance.received_time = Timestamp::from_unix_nanos(0);
  provenance.source_time = Timestamp::from_unix_nanos(100 * kSecond);
  const FreshnessAssessment assessment =
      model.assess(provenance, Timestamp::from_unix_nanos(100 * kSecond), MonotonicInstant::from_nanos(0));
  PO_REQUIRE(assessment.anomaly.has_value());
  PO_CHECK_EQ(*assessment.anomaly, ReasonCode::FutureTimestamp);
}

PO_TEST(freshness, a_backwards_monotonic_reading_is_refused) {
  FreshnessModel model;
  Provenance provenance = live_provenance();
  provenance.received_steady = MonotonicInstant::from_nanos(10 * kSecond);
  const FreshnessAssessment assessment =
      model.assess(provenance, Timestamp{}, MonotonicInstant::from_nanos(0));
  PO_CHECK_EQ(assessment.classification, FreshnessClass::Unknown);
  PO_CHECK_EQ(assessment.reason, ReasonCode::ClockSkewExceeded);
}

PO_TEST(freshness, per_source_budget_overrides_the_default) {
  FreshnessPolicy policy;
  policy.default_budget.fresh_within = seconds(1);
  policy.default_budget.aging_within = seconds(2);
  policy.default_budget.stale_within = seconds(3);
  FreshnessBudget override_budget;
  override_budget.fresh_within = seconds(1000);
  override_budget.aging_within = seconds(2000);
  override_budget.stale_within = seconds(3000);
  policy.per_source[SourceId("meter-1")] = override_budget;

  FreshnessModel model(policy);
  const Provenance provenance = live_provenance();
  PO_CHECK_EQ(model.assess(provenance, Timestamp{}, MonotonicInstant::from_nanos(100 * kSecond)).classification,
             FreshnessClass::Fresh);

  Provenance other = provenance;
  other.source = SourceId("meter-2");
  PO_CHECK_EQ(model.assess(other, Timestamp{}, MonotonicInstant::from_nanos(100 * kSecond)).classification,
             FreshnessClass::Expired);
}

PO_TEST(freshness, worse_orders_by_trustworthiness) {
  PO_CHECK_EQ(FreshnessModel::worse(FreshnessClass::Fresh, FreshnessClass::Aging), FreshnessClass::Aging);
  PO_CHECK_EQ(FreshnessModel::worse(FreshnessClass::Aging, FreshnessClass::Recovered),
              FreshnessClass::Aging);
  PO_CHECK_EQ(FreshnessModel::worse(FreshnessClass::Stale, FreshnessClass::Fresh), FreshnessClass::Stale);
  PO_CHECK_EQ(FreshnessModel::worse(FreshnessClass::Expired, FreshnessClass::Unknown),
              FreshnessClass::Expired);
}

PO_TEST(freshness, policy_validation_rejects_incoherent_bands) {
  FreshnessPolicy policy;
  policy.default_budget.fresh_within = seconds(10);
  policy.default_budget.aging_within = seconds(5);
  PO_REQUIRE_ERR(policy.validate(), ReasonCode::InvalidArgument);

  FreshnessPolicy negative;
  negative.max_clock_skew = Duration::from_raw(-1);
  PO_REQUIRE_ERR(negative.validate(), ReasonCode::InvalidArgument);
}

PO_TEST_MAIN()
