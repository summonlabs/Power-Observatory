// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/policy.hpp"

namespace po {
namespace {

[[nodiscard]] Status require_non_negative(Duration value, std::string_view what) {
  if (value.raw() < 0) {
    return fail(ReasonCode::InvalidArgument, std::string(what) + " must not be negative");
  }
  return ok_status();
}

[[nodiscard]] Status require_percentage(Ratio value, std::string_view what) {
  if (value.raw() < 0 || value.raw() > 1000000) {
    return fail(ReasonCode::InvalidArgument, std::string(what) + " must be within 0% and 100%");
  }
  return ok_status();
}

[[nodiscard]] Status require_positive(Power value, std::string_view what) {
  if (value.raw() <= 0) {
    return fail(ReasonCode::InvalidArgument, std::string(what) + " must be strictly positive");
  }
  return ok_status();
}

}  // namespace

Status FreshnessBudget::validate() const {
  Status status = require_non_negative(fresh_within, "freshness fresh_within");
  if (!status) {
    return status;
  }
  status = require_non_negative(aging_within, "freshness aging_within");
  if (!status) {
    return status;
  }
  status = require_non_negative(stale_within, "freshness stale_within");
  if (!status) {
    return status;
  }
  if (aging_within < fresh_within) {
    return fail(ReasonCode::InvalidArgument, "freshness aging_within must not be shorter than fresh_within");
  }
  if (stale_within < aging_within) {
    return fail(ReasonCode::InvalidArgument, "freshness stale_within must not be shorter than aging_within");
  }
  return ok_status();
}

Status FreshnessPolicy::validate() const {
  Status status = default_budget.validate();
  if (!status) {
    return status;
  }
  status = require_non_negative(max_clock_skew, "freshness max_clock_skew");
  if (!status) {
    return status;
  }
  for (const auto& entry : per_source) {
    if (entry.first.empty()) {
      return fail(ReasonCode::InvalidArgument, "freshness override has an empty source identifier");
    }
    status = entry.second.validate();
    if (!status) {
      return fail(status.code(), "freshness override for source " + entry.first.value() + ": " + status.detail());
    }
  }
  return ok_status();
}

const FreshnessBudget& FreshnessPolicy::budget_for(const SourceId& source) const noexcept {
  const auto found = per_source.find(source);
  return found == per_source.end() ? default_budget : found->second;
}

Status ImbalancePolicy::validate() const { return require_percentage(tolerance_ppm, "imbalance tolerance_ppm"); }

Status LossPolicy::validate() const { return require_percentage(max_loss_ppm, "loss max_loss_ppm"); }

Status QualityPolicy::validate() const {
  if (nominal_frequency.raw() <= 0) {
    return fail(ReasonCode::InvalidArgument, "quality nominal_frequency must be strictly positive");
  }
  if (frequency_tolerance.raw() < 0) {
    return fail(ReasonCode::InvalidArgument, "quality frequency_tolerance must not be negative");
  }
  if (nominal_voltage.raw() <= 0) {
    return fail(ReasonCode::InvalidArgument, "quality nominal_voltage must be strictly positive");
  }
  Status status = require_percentage(voltage_tolerance_ppm, "quality voltage_tolerance_ppm");
  if (!status) {
    return status;
  }
  status = require_percentage(minimum_power_factor_ppm, "quality minimum_power_factor_ppm");
  if (!status) {
    return status;
  }
  status = require_percentage(phase_imbalance_limit_ppm, "quality phase_imbalance_limit_ppm");
  if (!status) {
    return status;
  }
  status = require_positive(maximum_plausible_power, "quality maximum_plausible_power");
  if (!status) {
    return status;
  }
  if (maximum_plausible_current.raw() <= 0) {
    return fail(ReasonCode::InvalidArgument, "quality maximum_plausible_current must be strictly positive");
  }
  return ok_status();
}

Status ReservePolicy::validate() const {
  if (derate_ppm.raw() < 0 || derate_ppm.raw() > 1000000) {
    return fail(ReasonCode::InvalidArgument, "reserve derate_ppm must be within 0% and 100%");
  }
  if (minimum_headroom.raw() < 0) {
    return fail(ReasonCode::InvalidArgument, "reserve minimum_headroom must not be negative");
  }
  return ok_status();
}

Status FailoverPolicy::validate() const {
  Status status = require_non_negative(max_transfer_time, "failover max_transfer_time");
  if (!status) {
    return status;
  }
  status = require_non_negative(minimum_autonomy, "failover minimum_autonomy");
  if (!status) {
    return status;
  }
  return require_percentage(minimum_peer_margin_ppm, "failover minimum_peer_margin_ppm");
}

Status ObservationPolicy::validate() const {
  Status status = freshness.validate();
  if (!status) {
    return status;
  }
  status = imbalance.validate();
  if (!status) {
    return status;
  }
  status = loss.validate();
  if (!status) {
    return status;
  }
  status = quality.validate();
  if (!status) {
    return status;
  }
  status = reserve.validate();
  if (!status) {
    return status;
  }
  return failover.validate();
}

ObservationPolicy default_policy() { return ObservationPolicy{}; }

ObservationPolicy policy_for_synthetic_lab() {
  ObservationPolicy policy;
  policy.freshness.default_budget.fresh_within = seconds(30);
  policy.freshness.default_budget.aging_within = seconds(90);
  policy.freshness.default_budget.stale_within = seconds(600);
  return policy;
}

}  // namespace po
