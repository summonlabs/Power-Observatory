// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/quality.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>

namespace po {
namespace {

[[nodiscard]] QuantityRep absolute_value(QuantityRep value) noexcept {
  return value < 0 ? (value == std::numeric_limits<QuantityRep>::min() ? std::numeric_limits<QuantityRep>::max()
                                                                       : -value)
                   : value;
}

[[nodiscard]] std::vector<EntityRef> collect_scope(const ObservationContext& context,
                                                   const std::optional<EntityRef>& scope) {
  if (!scope.has_value()) {
    return context.topology.entity_order();
  }
  std::vector<EntityRef> result;
  result.push_back(*scope);
  std::set<EntityRef> visited;
  visited.insert(*scope);
  // A redundancy group is a grouping of its members, not a node on the
  // wiring. Reporting it as a node would count its members twice, so it is
  // reported through the reserve and failover engines instead.
  if (scope->kind() == EntityKind::RedundancyGroup) {
    result.clear();
  }
  const std::size_t budget = (context.topology.size() + 1) * 4;
  std::size_t index = 0;
  while (index < result.size() && result.size() < budget) {
    for (const EntityRef& child : context.topology.children(result[index])) {
      if (visited.insert(child).second) {
        result.push_back(child);
      }
    }
    ++index;
  }
  std::sort(result.begin(), result.end());
  return result;
}

void record(QualityReport& report, Explanation& explanation, ReasonCode code, const EntityRef& entity,
            MeasurementKind kind, Phase phase, std::string detail, std::optional<EvidenceRef> evidence) {
  QualityFinding finding;
  finding.code = code;
  finding.severity = po::severity(code);
  finding.entity = entity;
  finding.kind = kind;
  finding.phase = phase;
  finding.detail = std::move(detail);
  finding.evidence = std::move(evidence);

  std::vector<EvidenceRef> references;
  if (finding.evidence.has_value()) {
    references.push_back(*finding.evidence);
  }
  explanation.add(code, entity.to_string(), finding.detail, std::move(references));
  report.findings.push_back(std::move(finding));
}

[[nodiscard]] bool outside_ratio(QuantityRep deviation, QuantityRep reference, Ratio limit) noexcept {
  if (reference <= 0) {
    return false;
  }
  QuantityRep scaled{};
  if (!checked_mul_div(absolute_value(deviation), 1000000, reference, scaled)) {
    return true;
  }
  return absolute_value(scaled) > limit.raw();
}

}  // namespace

Outcome<QualityReport> compute_quality(const ObservationContext& context, const QualityQuery& query) {
  QualityReport report;
  Explanation explanation;
  report.computed_at = context.wall_now;

  if (query.scope.has_value() && !context.topology.contains(*query.scope)) {
    explanation.add(ReasonCode::UnknownEntity, query.scope->to_string(),
                    "the declared topology contains no such entity");
    return Outcome<QualityReport>(
        Error(ReasonCode::UnknownEntity, "quality query names an entity that the topology does not declare"),
        std::move(explanation));
  }

  const std::vector<EntityRef> scope = collect_scope(context, query.scope);
  const QualityPolicy& policy = context.policy.quality;

  // Phase imbalance needs all three phases of the same entity and quantity, so
  // the per-entity values are collected first.
  std::map<std::tuple<EntityRef, MeasurementKind>, std::map<Phase, QuantityRep>> phase_values;

  for (const EntityRef& entity : scope) {
    for (const Measurement* measurement : context.evidence.for_entity(entity)) {
      const MeasurementView view = select_measurement(context, entity, measurement->kind(), measurement->phase);
      if (view.measurement != measurement) {
        continue;  // A more trustworthy measurement exists for this key.
      }
      if (!view.usable()) {
        ++report.measurements_skipped_stale;
        explanation.add(view.freshness.reason, entity.to_string(),
                        std::string(po::to_string(measurement->kind())) + " " +
                            std::string(po::to_string(measurement->phase)) + " was not assessed because " +
                            view.freshness.detail);
        continue;
      }
      ++report.measurements_assessed;
      const std::optional<EvidenceRef> reference = view.reference();
      const MeasurementKind kind = measurement->kind();
      const Phase phase = measurement->phase;

      if (view.freshness.anomaly.has_value()) {
        explanation.add(*view.freshness.anomaly, entity.to_string(), view.freshness.anomaly_detail);
      }

      switch (kind) {
        case MeasurementKind::Frequency: {
          const Frequency value = std::get<Frequency>(measurement->value);
          QuantityRep deviation{};
          if (!checked_sub(value.raw(), policy.nominal_frequency.raw(), deviation)) {
            record(report, explanation, ReasonCode::ArithmeticOverflow, entity, kind, phase,
                   "frequency deviation overflowed 64-bit range", reference);
            break;
          }
          if (absolute_value(deviation) > policy.frequency_tolerance.raw()) {
            record(report, explanation, ReasonCode::FrequencyOutOfRange, entity, kind, phase,
                   "measured " + value.to_string() + " against a nominal " +
                       policy.nominal_frequency.to_string() + " with a tolerance of " +
                       policy.frequency_tolerance.to_string(),
                   reference);
          }
          break;
        }
        case MeasurementKind::Voltage: {
          const Voltage value = std::get<Voltage>(measurement->value);
          QuantityRep deviation{};
          if (!checked_sub(value.raw(), policy.nominal_voltage.raw(), deviation)) {
            record(report, explanation, ReasonCode::ArithmeticOverflow, entity, kind, phase,
                   "voltage deviation overflowed 64-bit range", reference);
            break;
          }
          if (outside_ratio(deviation, policy.nominal_voltage.raw(), policy.voltage_tolerance_ppm)) {
            record(report, explanation, ReasonCode::VoltageOutOfRange, entity, kind, phase,
                   "measured " + value.to_string() + " against a nominal " + policy.nominal_voltage.to_string() +
                       " with a tolerance of " + policy.voltage_tolerance_ppm.to_string(),
                   reference);
          }
          break;
        }
        case MeasurementKind::PowerFactor: {
          const Ratio value = std::get<Ratio>(measurement->value);
          if (value.raw() < 0 || value.raw() > 1000000) {
            record(report, explanation, ReasonCode::PowerFactorOutOfRange, entity, kind, phase,
                   "measured power factor " + value.to_string() + " lies outside the physically possible 0%..100%",
                   reference);
          } else if (value.raw() < policy.minimum_power_factor_ppm.raw()) {
            record(report, explanation, ReasonCode::PowerFactorOutOfRange, entity, kind, phase,
                   "measured power factor " + value.to_string() + " is below the policy minimum of " +
                       policy.minimum_power_factor_ppm.to_string(),
                   reference);
          }
          break;
        }
        case MeasurementKind::ActivePower:
        case MeasurementKind::ApparentPower:
        case MeasurementKind::ReactivePower:
        case MeasurementKind::Energy: {
          const QuantityRep raw = raw_value(measurement->value);
          if (absolute_value(raw) > policy.maximum_plausible_power.raw()) {
            record(report, explanation, ReasonCode::NumericExtreme, entity, kind, phase,
                   "value " + value_string(measurement->value) + " exceeds the plausibility ceiling of " +
                       policy.maximum_plausible_power.to_string(),
                   reference);
          }
          if (kind == MeasurementKind::ActivePower && raw < 0 && entity.kind() == EntityKind::Load) {
            record(report, explanation, ReasonCode::NegativeQuantity, entity, kind, phase,
                   "a declared load reports negative active power " + value_string(measurement->value) +
                       ", which means power is leaving the load",
                   reference);
          }
          break;
        }
        case MeasurementKind::Current: {
          const Current value = std::get<Current>(measurement->value);
          if (absolute_value(value.raw()) > policy.maximum_plausible_current.raw()) {
            record(report, explanation, ReasonCode::NumericExtreme, entity, kind, phase,
                   "value " + value.to_string() + " exceeds the plausibility ceiling of " +
                       policy.maximum_plausible_current.to_string(),
                   reference);
          }
          break;
        }
        case MeasurementKind::Temperature: {
          const Temperature value = std::get<Temperature>(measurement->value);
          if (value.raw() < -50000 || value.raw() > 150000) {
            record(report, explanation, ReasonCode::ValueOutOfRange, entity, kind, phase,
                   "temperature " + value.to_string() + " is outside the range this runtime will accept as plant data",
                   reference);
          }
          break;
        }
      }

      if (kind == MeasurementKind::Voltage || kind == MeasurementKind::Current) {
        phase_values[std::make_tuple(entity, kind)][phase] = raw_value(measurement->value);
      }
    }
  }

  for (const auto& entry : phase_values) {
    const EntityRef& entity = std::get<0>(entry.first);
    const MeasurementKind kind = std::get<1>(entry.first);
    const std::map<Phase, QuantityRep>& phases = entry.second;
    const auto phase_a = phases.find(Phase::A);
    const auto phase_b = phases.find(Phase::B);
    const auto phase_c = phases.find(Phase::C);
    if (phase_a == phases.end() || phase_b == phases.end() || phase_c == phases.end()) {
      record(report, explanation, ReasonCode::MissingMeasurement, entity, kind, Phase::Total,
             "phase imbalance cannot be assessed because not all of phases A, B and C carry usable evidence",
             std::nullopt);
      continue;
    }
    Accumulator<PowerUnitTag> accumulator;
    accumulator.add(Power::from_raw(phase_a->second));
    accumulator.add(Power::from_raw(phase_b->second));
    accumulator.add(Power::from_raw(phase_c->second));
    QuantityRep sum{};
    if (!checked_add(phase_a->second, phase_b->second, sum) || !checked_add(sum, phase_c->second, sum)) {
      record(report, explanation, ReasonCode::ArithmeticOverflow, entity, kind, Phase::Total,
             "phase sum overflowed 64-bit range", std::nullopt);
      continue;
    }
    QuantityRep mean{};
    if (!checked_div(sum, 3, mean) || mean <= 0) {
      record(report, explanation, ReasonCode::ValueOutOfRange, entity, kind, Phase::Total,
             "phase mean is not positive, so imbalance cannot be expressed as a fraction", std::nullopt);
      continue;
    }
    QuantityRep worst{};
    for (const QuantityRep value : {phase_a->second, phase_b->second, phase_c->second}) {
      QuantityRep deviation{};
      if (!checked_sub(value, mean, deviation)) {
        worst = std::numeric_limits<QuantityRep>::max();
        break;
      }
      const QuantityRep magnitude = absolute_value(deviation);
      if (magnitude > worst) {
        worst = magnitude;
      }
    }
    if (outside_ratio(worst, mean, policy.phase_imbalance_limit_ppm)) {
      record(report, explanation, ReasonCode::PhaseImbalanceExceeded, entity, kind, Phase::Total,
             "worst phase deviation of " + Power::from_raw(worst).to_string() + " against a mean of " +
                 Power::from_raw(mean).to_string() + " exceeds the policy limit of " +
                 policy.phase_imbalance_limit_ppm.to_string(),
             std::nullopt);
    }
  }

  std::sort(report.findings.begin(), report.findings.end());
  report.findings.erase(std::unique(report.findings.begin(), report.findings.end()), report.findings.end());

  if (report.measurements_assessed == 0) {
    if (report.measurements_skipped_stale > 0) {
      report.state = EvidenceState::Stale;
      explanation.add(ReasonCode::StaleEvidence, std::string("<quality>"),
                      "every measurement in scope is present but no longer usable");
    } else {
      report.state = EvidenceState::Unknown;
      explanation.add(ReasonCode::NoEvidence, std::string("<quality>"),
                      "no measurement in scope could be assessed");
    }
  } else if (report.measurements_skipped_stale > 0) {
    report.state = EvidenceState::Indeterminate;
    explanation.add(ReasonCode::StaleEvidence, std::string("<quality>"),
                    std::to_string(report.measurements_skipped_stale) +
                        " measurement(s) were skipped because their evidence is no longer usable");
  } else {
    report.state = EvidenceState::Known;
    if (report.findings.empty()) {
      explanation.add(ReasonCode::QualityNominal, std::string("<quality>"),
                      std::to_string(report.measurements_assessed) +
                          " measurement(s) assessed with no finding");
    }
  }

  explanation.canonicalize();
  report.explanation = explanation;
  return Outcome<QualityReport>(std::move(report), std::move(explanation));
}

}  // namespace po