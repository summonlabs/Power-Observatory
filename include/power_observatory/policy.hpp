// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <map>

#include "power_observatory/evidence.hpp"
#include "power_observatory/quantity.hpp"
#include "power_observatory/result.hpp"
#include "power_observatory/strong.hpp"

namespace po {

// Three explicit age bands. A source declares how long its evidence may be
// treated as fresh, as aging, and as usable at all; beyond the last band the
// evidence is expired and must not contribute to any answer.
struct FreshnessBudget {
  Duration fresh_within{seconds(5)};
  Duration aging_within{seconds(15)};
  Duration stale_within{seconds(60)};

  [[nodiscard]] Status validate() const;
};

struct FreshnessPolicy {
  FreshnessBudget default_budget{};
  // Per-source overrides. A source with no entry uses default_budget.
  std::map<SourceId, FreshnessBudget> per_source;
  // How far a source's own timestamp may disagree with local arrival before the
  // disagreement itself is reported.
  Duration max_clock_skew{seconds(2)};

  [[nodiscard]] const FreshnessBudget& budget_for(const SourceId& source) const noexcept;
  [[nodiscard]] Status validate() const;
};

struct ImbalancePolicy {
  // Residual beyond this fraction of the parent reading is a finding. The
  // default is deliberately generous: a residual at a bus is the sum of
  // current-transformer error, sampling skew, and the conversion losses of
  // every stage between the two meters, none of which is an imbalance.
  Ratio tolerance_ppm{Ratio::from_raw(50000)};
  // Residuals below this absolute value are treated as within tolerance
  // regardless of the fraction, so that near-zero parents do not produce noise.
  Power absolute_floor{Power::from_raw(10000)};

  [[nodiscard]] Status validate() const;
};

struct LossPolicy {
  Ratio max_loss_ppm{Ratio::from_raw(100000)};
  Power absolute_floor{Power::from_raw(5000)};

  [[nodiscard]] Status validate() const;
};

struct QualityPolicy {
  Frequency nominal_frequency{millihertz(50000)};
  Frequency frequency_tolerance{millihertz(500)};
  Voltage nominal_voltage{millivolts(230000)};
  Ratio voltage_tolerance_ppm{Ratio::from_raw(100000)};
  Ratio minimum_power_factor_ppm{Ratio::from_raw(900000)};
  Ratio phase_imbalance_limit_ppm{Ratio::from_raw(30000)};
  // Values beyond these magnitudes are treated as implausible telemetry rather
  // than as large real measurements.
  Power maximum_plausible_power{Power::from_raw(1000000000000)};
  Current maximum_plausible_current{Current::from_raw(100000000)};

  [[nodiscard]] Status validate() const;
};

struct ReservePolicy {
  Ratio derate_ppm{Ratio::from_raw(1000000)};
  Power minimum_headroom{Power::from_raw(0)};
  bool require_declared_redundancy{true};

  [[nodiscard]] Status validate() const;
};

struct FailoverPolicy {
  Duration max_transfer_time{seconds(10)};
  Duration minimum_autonomy{seconds(300)};
  Ratio minimum_peer_margin_ppm{Ratio::from_raw(200000)};
  bool require_peer_live{true};
  bool require_sync_evidence{true};

  [[nodiscard]] Status validate() const;
};

struct ObservationPolicy {
  FreshnessPolicy freshness;
  ImbalancePolicy imbalance;
  LossPolicy loss;
  QualityPolicy quality;
  ReservePolicy reserve;
  FailoverPolicy failover;

  [[nodiscard]] Status validate() const;
};

[[nodiscard]] ObservationPolicy default_policy();
[[nodiscard]] ObservationPolicy policy_for_synthetic_lab();

}  // namespace po
