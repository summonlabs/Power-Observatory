// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/attribution.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace po {
namespace {

[[nodiscard]] QuantityRep absolute_value(QuantityRep value) noexcept {
  return value < 0 ? (value == std::numeric_limits<QuantityRep>::min() ? std::numeric_limits<QuantityRep>::max()
                                                                       : -value)
                   : value;
}

// The allowance for a residual: the larger of the policy floor and the policy
// fraction of the parent reading.
[[nodiscard]] QuantityRep residual_allowance(Power parent, Ratio tolerance_ppm, Power absolute_floor) noexcept {
  QuantityRep scaled{};
  if (!checked_mul_div(absolute_value(parent.raw()), tolerance_ppm.raw(), 1000000, scaled)) {
    return std::numeric_limits<QuantityRep>::max();
  }
  const QuantityRep floor_value = absolute_value(absolute_floor.raw());
  return scaled > floor_value ? scaled : floor_value;
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

}  // namespace

Outcome<AttributionReport> compute_attribution(const ObservationContext& context, const AttributionQuery& query) {
  AttributionReport report;
  Explanation explanation;
  report.computed_at = context.wall_now;

  if (query.scope.has_value() && !context.topology.contains(*query.scope)) {
    explanation.add(ReasonCode::UnknownEntity, query.scope->to_string(),
                    "the declared topology contains no such entity");
    return Outcome<AttributionReport>(
        Error(ReasonCode::UnknownEntity, "attribution query names an entity that the topology does not declare"),
        std::move(explanation));
  }

  const std::vector<EntityRef> scope = collect_scope(context, query.scope);
  bool saw_conflict = false;
  bool saw_indeterminate = false;
  bool saw_stale = false;

  if (query.include_imbalance) {
    for (const EntityRef& parent : scope) {
      if (parent.kind() == EntityKind::RedundancyGroup) {
        continue;  // A grouping has no meter of its own and no wiring of its own.
      }
      const std::vector<EntityRef> children = context.topology.children(parent);
      if (children.empty()) {
        continue;
      }

      ImbalanceRecord record;
      record.parent = parent;
      record.tolerance_ppm = context.policy.imbalance.tolerance_ppm;
      record.absolute_floor = context.policy.imbalance.absolute_floor;

      const MeasurementView view = select_active_power(context, parent);
      if (!view.present()) {
        record.state = EvidenceState::Unknown;
        saw_indeterminate = true;
        explanation.add(ReasonCode::NotMeasured, parent.to_string(),
                        "the parent carries no active power measurement, so no residual can be formed");
        report.imbalances.push_back(std::move(record));
        continue;
      }
      record.evidence.push_back(view.reference());
      if (!view.usable()) {
        record.state = to_evidence_state(view.freshness.classification);
        saw_stale = true;
        explanation.add(view.freshness.reason, parent.to_string(),
                        "the parent measurement is present but " + view.freshness.detail);
        report.imbalances.push_back(std::move(record));
        continue;
      }

      const Result<Power> parent_value = as_power(*view.measurement);
      if (!parent_value) {
        record.state = EvidenceState::Unsupported;
        saw_indeterminate = true;
        explanation.add(parent_value.code(), parent.to_string(), parent_value.detail());
        report.imbalances.push_back(std::move(record));
        continue;
      }

      Accumulator<PowerUnitTag> child_total;
      bool child_conflict = false;
      for (const EntityRef& child : children) {
        const MeasurementView child_view = select_active_power(context, child);
        if (!child_view.usable()) {
          record.unmeasured_children.push_back(child);
          explanation.add(ReasonCode::MissingMeasurement, child.to_string(),
                          "no usable active power measurement, so this branch cannot be attributed");
          continue;
        }
        const Result<Power> child_value = as_power(*child_view.measurement);
        if (!child_value) {
          record.unmeasured_children.push_back(child);
          continue;
        }
        record.measured_children.push_back(child);
        record.evidence.push_back(child_view.reference());
        child_total.add(child_value.value());
        if (child_view.freshness.anomaly.has_value()) {
          explanation.add(*child_view.freshness.anomaly, child.to_string(), child_view.freshness.anomaly_detail);
        }
      }
      (void)child_conflict;

      QuantityRep residual_raw{};
      if (!checked_sub(parent_value.value().raw(), child_total.saturated_total().raw(), residual_raw)) {
        record.state = EvidenceState::Indeterminate;
        saw_indeterminate = true;
        explanation.add(ReasonCode::ArithmeticOverflow, parent.to_string(),
                        "parent minus children overflowed 64-bit range");
        report.imbalances.push_back(std::move(record));
        continue;
      }

      record.parent_measured = parent_value.value();
      record.children_total = child_total.saturated_total();
      record.residual = Power::from_raw(residual_raw);
      if (!record.parent_measured.is_zero()) {
        const Result<Ratio> share = ratio_ppm_of(record.residual, record.parent_measured);
        if (share) {
          record.residual_ppm = share.value();
        }
      }

      const QuantityRep allowance = residual_allowance(record.parent_measured, record.tolerance_ppm,
                                                       record.absolute_floor);
      record.within_tolerance = absolute_value(residual_raw) <= allowance;

      if (record.within_tolerance) {
        if (record.unmeasured_children.empty()) {
          record.state = EvidenceState::Known;
        } else {
          record.state = EvidenceState::Indeterminate;
          saw_indeterminate = true;
        }
        explanation.add(ReasonCode::WithinTolerance, parent.to_string(),
                        "residual " + record.residual.to_string() + " is within the allowance of " +
                            Power::from_raw(allowance).to_string());
      } else {
        ++report.unattributed_imbalances;
        if (record.unmeasured_children.empty()) {
          record.state = EvidenceState::Conflicting;
          saw_conflict = true;
        } else {
          record.state = EvidenceState::Indeterminate;
          saw_indeterminate = true;
        }
        explanation.add(ReasonCode::ImbalanceUnattributed, parent.to_string(),
                        "measured " + record.parent_measured.to_string() + " at the parent but " +
                            record.children_total.to_string() + " across " +
                            std::to_string(record.measured_children.size()) + " measured child or children, leaving " +
                            record.residual.to_string() + " unattributed against an allowance of " +
                            Power::from_raw(allowance).to_string());
      }

      std::sort(record.evidence.begin(), record.evidence.end());
      record.evidence.erase(std::unique(record.evidence.begin(), record.evidence.end()), record.evidence.end());
      report.imbalances.push_back(std::move(record));
    }
  }

  if (query.include_losses) {
    Accumulator<PowerUnitTag> measured_losses;
    for (const EntityRef& node : scope) {
      if (node.kind() != EntityKind::Ups && node.kind() != EntityKind::Pdu) {
        continue;
      }

      LossRecord record;
      record.node = node;
      record.limit_ppm = context.policy.loss.max_loss_ppm;
      record.absolute_floor = context.policy.loss.absolute_floor;

      std::vector<EntityRef> input_side;
      std::vector<EntityRef> output_side;

      if (node.kind() == EntityKind::Ups) {
        const Ups* unit = context.topology.ups(UpsId(node.id()));
        if (unit == nullptr) {
          continue;
        }
        record.declared_efficiency_ppm = unit->declared_efficiency_ppm;
        input_side.push_back(node);
        if (unit->input.has_value()) {
          input_side.push_back(EntityRef::bus(*unit->input));
        }
        for (const BusId& output : unit->outputs) {
          output_side.push_back(EntityRef::bus(output));
        }
      } else {
        const Pdu* unit = context.topology.pdu(PduId(node.id()));
        if (unit == nullptr) {
          continue;
        }
        input_side.push_back(node);
        if (unit->input.has_value()) {
          input_side.push_back(EntityRef::bus(*unit->input));
        }
        for (const Circuit& circuit : context.topology.circuits()) {
          if (circuit.pdu.has_value() && *circuit.pdu == PduId(node.id())) {
            output_side.push_back(EntityRef::circuit(circuit.id));
          }
        }
      }

      if (input_side.empty()) {
        record.state = EvidenceState::Unsupported;
        explanation.add(ReasonCode::LossModelUnavailable, node.to_string(),
                        "the declared wiring does not name an input for this node, so a conversion loss cannot be "
                        "formed");
        report.losses.push_back(std::move(record));
        continue;
      }
      if (output_side.empty()) {
        record.state = EvidenceState::Unsupported;
        explanation.add(ReasonCode::LossModelUnavailable, node.to_string(),
                        "the declared wiring does not name any output for this node, so a conversion loss cannot be "
                        "formed");
        report.losses.push_back(std::move(record));
        continue;
      }

      // The candidates are ordered by preference, not summed: the node's own
      // meter measures what it draws, and the input bus is only a substitute
      // when no such meter exists.
      Accumulator<PowerUnitTag> input_total;
      bool input_ok = false;
      for (const EntityRef& candidate : input_side) {
        const MeasurementView view = select_active_power(context, candidate);
        if (!view.usable()) {
          continue;
        }
        const Result<Power> value = as_power(*view.measurement);
        if (!value) {
          continue;
        }
        if (candidate == node) {
          input_total.add(value.value());
          record.evidence.push_back(view.reference());
          input_ok = true;
          break;
        }
        if (!input_ok) {
          input_total.add(value.value());
          record.evidence.push_back(view.reference());
          input_ok = true;
          break;
        }
      }
      if (!input_ok) {
        explanation.add(ReasonCode::NotMeasured, node.to_string(),
                        "neither the node's own meter nor its input bus carries a usable active power measurement");
      }

      std::size_t measured_outputs = 0;
      Accumulator<PowerUnitTag> output_total;
      for (const EntityRef& candidate : output_side) {
        const MeasurementView view = select_active_power(context, candidate);
        if (!view.usable()) {
          continue;
        }
        const Result<Power> value = as_power(*view.measurement);
        if (!value) {
          continue;
        }
        ++measured_outputs;
        output_total.add(value.value());
        record.evidence.push_back(view.reference());
      }

      if (!input_ok || measured_outputs == 0) {
        record.state = EvidenceState::Unknown;
        saw_indeterminate = true;
        explanation.add(ReasonCode::LossModelUnavailable, node.to_string(),
                        "the conversion loss for this node is unknown because " +
                            std::string(input_ok ? "no output" : "no input") +
                            " side measurement is usable; only the declared efficiency is available and declared "
                            "efficiency is not a measurement");
        report.losses.push_back(std::move(record));
        continue;
      }
      if (measured_outputs != output_side.size()) {
        explanation.add(ReasonCode::MissingMeasurement, node.to_string(),
                        std::to_string(output_side.size() - measured_outputs) + " of " +
                            std::to_string(output_side.size()) +
                            " outputs carry no usable measurement, so the loss is a lower bound");
      }

      record.input = input_total.saturated_total();
      record.output = output_total.saturated_total();
      QuantityRep loss_raw{};
      if (!checked_sub(record.input.raw(), record.output.raw(), loss_raw)) {
        record.state = EvidenceState::Indeterminate;
        saw_indeterminate = true;
        explanation.add(ReasonCode::ArithmeticOverflow, node.to_string(),
                        "input minus output overflowed 64-bit range");
        report.losses.push_back(std::move(record));
        continue;
      }
      record.loss = Power::from_raw(loss_raw);

      if (!record.input.is_zero()) {
        const Result<Ratio> share = ratio_ppm_of(record.loss, record.input);
        if (share) {
          record.loss_ppm = share.value();
        }
      }

      if (record.loss.is_negative()) {
        record.state = EvidenceState::Conflicting;
        saw_conflict = true;
        explanation.add(ReasonCode::NegativeQuantity, node.to_string(),
                        "measured output " + record.output.to_string() + " exceeds measured input " +
                            record.input.to_string() +
                            "; a conversion stage cannot create power, so at least one meter is wrong");
        report.losses.push_back(std::move(record));
        continue;
      }

      const QuantityRep allowance =
          residual_allowance(record.input, record.limit_ppm, record.absolute_floor);
      record.within_limit = absolute_value(loss_raw) <= allowance;
      record.state = EvidenceState::Known;
      measured_losses.add(record.loss);

      if (record.within_limit) {
        explanation.add(ReasonCode::NoLossDetected, node.to_string(),
                        "conversion loss " + record.loss.to_string() + " is within the allowance of " +
                            Power::from_raw(allowance).to_string());
      } else {
        explanation.add(ReasonCode::LossExceeded, node.to_string(),
                        "conversion loss " + record.loss.to_string() + " exceeds the allowance of " +
                            Power::from_raw(allowance).to_string() + " against a measured input of " +
                            record.input.to_string());
      }

      std::sort(record.evidence.begin(), record.evidence.end());
      record.evidence.erase(std::unique(record.evidence.begin(), record.evidence.end()), record.evidence.end());
      report.losses.push_back(std::move(record));
    }

    const std::size_t measurable = static_cast<std::size_t>(
        std::count_if(report.losses.begin(), report.losses.end(),
                      [](const LossRecord& entry) { return entry.state == EvidenceState::Known; }));
    if (measurable > 0 && !measured_losses.overflowed()) {
      report.total_measured_loss = measured_losses.saturated_total();
    } else if (measurable > 0) {
      explanation.add(ReasonCode::ArithmeticOverflow, std::string("<attribution>"),
                      "the total measured loss overflowed 64-bit range");
    }
  }

  std::sort(report.imbalances.begin(), report.imbalances.end());
  std::sort(report.losses.begin(), report.losses.end());

  if (saw_conflict) {
    report.state = EvidenceState::Conflicting;
  } else if (saw_indeterminate) {
    report.state = EvidenceState::Indeterminate;
  } else if (saw_stale) {
    report.state = EvidenceState::Stale;
  } else if (!report.imbalances.empty() || !report.losses.empty()) {
    report.state = EvidenceState::Known;
  } else {
    report.state = EvidenceState::Unknown;
    explanation.add(ReasonCode::NoEvidence, std::string("<attribution>"),
                    "no parent/child pair or conversion stage in scope could be assessed");
  }

  explanation.canonicalize();
  report.explanation = explanation;
  return Outcome<AttributionReport>(std::move(report), std::move(explanation));
}

}  // namespace po