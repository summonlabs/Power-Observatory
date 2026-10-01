// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/flow.hpp"

#include <algorithm>
#include <set>
#include <string>

namespace po {

std::string_view to_string(FlowDirection direction) noexcept {
  switch (direction) {
    case FlowDirection::Unknown:
      return "unknown";
    case FlowDirection::Importing:
      return "importing";
    case FlowDirection::Exporting:
      return "exporting";
    case FlowDirection::Idle:
      return "idle";
  }
  return "unknown";
}

namespace {

// A redundancy group is a grouping of its members, not a node on the wiring.
// Reporting it as a node would count every member twice, so groups are reported
// through the reserve and failover engines instead.
[[nodiscard]] bool is_electrical_node(const EntityRef& entity) noexcept {
  return entity.kind() != EntityKind::RedundancyGroup;
}

[[nodiscard]] std::vector<EntityRef> collect_scope(const ObservationContext& context, const FlowQuery& query) {
  if (!query.scope.has_value()) {
    std::vector<EntityRef> all;
    for (const EntityRef& entity : context.topology.entity_order()) {
      if (is_electrical_node(entity)) {
        all.push_back(entity);
      }
    }
    return all;
  }

  std::vector<EntityRef> result;
  if (is_electrical_node(*query.scope)) {
    result.push_back(*query.scope);
  }
  if (!query.include_children) {
    return result;
  }

  std::set<EntityRef> visited;
  visited.insert(*query.scope);
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

Outcome<FlowReport> compute_flow(const ObservationContext& context, const FlowQuery& query) {
  FlowReport report;
  Explanation explanation;
  report.computed_at = context.wall_now;

  if (query.scope.has_value() && !context.topology.contains(*query.scope)) {
    explanation.add(ReasonCode::UnknownEntity, query.scope->to_string(),
                    "the declared topology contains no such entity");
    return Outcome<FlowReport>(
        Error(ReasonCode::UnknownEntity, "flow query names an entity that the topology does not declare"),
        std::move(explanation));
  }

  const std::string scope_label = query.scope.has_value() ? query.scope->to_string() : std::string("<whole-model>");
  const std::vector<EntityRef> nodes = collect_scope(context, query);
  report.nodes.reserve(nodes.size());

  std::size_t missing = 0;
  std::size_t stale = 0;
  std::size_t recovered = 0;

  for (const EntityRef& entity : nodes) {
    FlowNode node;
    node.entity = entity;
    const MeasurementView view = select_active_power(context, entity);

    if (!view.present()) {
      node.state = EvidenceState::Unknown;
      ++missing;
      explanation.add(ReasonCode::NotMeasured, entity.to_string(),
                      "no active power measurement was admitted for this entity");
    } else {
      node.freshness = view.freshness.classification;
      node.evidence.push_back(view.reference());
      const Result<Power> value = as_power(*view.measurement);
      if (!value) {
        node.state = EvidenceState::Unsupported;
        ++missing;
        explanation.add(value.code(), entity.to_string(), value.detail());
      } else {
        node.active_power = value.value();
        node.state = to_evidence_state(view.freshness.classification);
        node.counted = view.usable();
        if (view.usable()) {
          node.direction = value.value().is_positive() ? FlowDirection::Importing
                           : value.value().is_negative() ? FlowDirection::Exporting
                                                         : FlowDirection::Idle;
          ++report.measured_nodes;
          if (view.freshness.classification == FreshnessClass::Recovered) {
            ++recovered;
          }
        } else {
          ++stale;
          explanation.add(view.freshness.reason, entity.to_string(),
                          "measured " + value.value().to_string() + " but " + view.freshness.detail +
                              "; the value is reported but is not counted toward the total");
        }
        if (view.freshness.anomaly.has_value()) {
          explanation.add(*view.freshness.anomaly, entity.to_string(), view.freshness.anomaly_detail);
        }
      }
    }
    report.nodes.push_back(std::move(node));
  }

  // Second pass: residuals and the counting set.
  std::vector<std::size_t> counting;
  for (std::size_t index = 0; index < report.nodes.size(); ++index) {
    FlowNode& node = report.nodes[index];
    const std::vector<EntityRef> children = context.topology.children(node.entity);
    if (children.empty()) {
      if (node.counted) {
        counting.push_back(index);
      }
      continue;
    }

    Accumulator<PowerUnitTag> child_total;
    for (const EntityRef& child : children) {
      const MeasurementView child_view = select_active_power(context, child);
      if (!child_view.usable()) {
        node.unmeasured_children.push_back(child);
        continue;
      }
      const Result<Power> child_value = as_power(*child_view.measurement);
      if (!child_value) {
        node.unmeasured_children.push_back(child);
        continue;
      }
      node.measured_children.push_back(child);
      child_total.add(child_value.value());
    }
    std::sort(node.measured_children.begin(), node.measured_children.end());
    std::sort(node.unmeasured_children.begin(), node.unmeasured_children.end());

    if (node.counted && node.active_power.has_value() && node.measured_children.empty()) {
      counting.push_back(index);
    }

    if (node.counted && node.active_power.has_value() && !node.measured_children.empty()) {
      QuantityRep residual_raw{};
      if (checked_sub(node.active_power->raw(), child_total.saturated_total().raw(), residual_raw)) {
        node.child_residual = Power::from_raw(residual_raw);
        if (!node.active_power->is_zero()) {
          const Result<Ratio> share = ratio_ppm_of(*node.child_residual, *node.active_power);
          if (share) {
            node.child_residual_ppm = share.value();
          }
        }
      } else {
        explanation.add(ReasonCode::ArithmeticOverflow, node.entity.to_string(),
                        "parent minus children residual overflowed 64-bit range");
      }
    } else if (!node.unmeasured_children.empty() && node.active_power.has_value()) {
      explanation.add(ReasonCode::MissingMeasurement, node.entity.to_string(),
                      std::to_string(node.unmeasured_children.size()) +
                          " downstream entity or entities carry no usable active power measurement, so the "
                          "residual cannot be attributed");
    }
  }

  Accumulator<PowerUnitTag> total;
  for (const std::size_t index : counting) {
    if (report.nodes[index].active_power.has_value()) {
      total.add(*report.nodes[index].active_power);
    }
  }
  if (!counting.empty()) {
    if (total.overflowed()) {
      explanation.add(ReasonCode::ArithmeticOverflow, scope_label,
                      "the observed load total overflowed 64-bit range and is therefore not published");
    } else {
      report.total_observed_load = total.saturated_total();
      if (!report.total_observed_load->is_zero()) {
        for (const std::size_t index : counting) {
          FlowNode& node = report.nodes[index];
          if (!node.active_power.has_value()) {
            continue;
          }
          const Result<Ratio> share = ratio_ppm_of(*node.active_power, *report.total_observed_load);
          if (share) {
            node.share_of_total_ppm = share.value();
          }
        }
      }
    }
  }

  if (report.measured_nodes == 0) {
    report.state = EvidenceState::Unknown;
    explanation.add(ReasonCode::NoEvidence, scope_label,
                    "no usable active power measurement was available anywhere in scope");
  } else if (missing == 0 && stale == 0) {
    report.state = recovered > 0 ? EvidenceState::Recovered : EvidenceState::Known;
    explanation.add(recovered > 0 ? ReasonCode::RecoveredNotFresh : ReasonCode::FlowEstablished, scope_label,
                    std::to_string(report.measured_nodes) + " entit" +
                        (report.measured_nodes == 1 ? "y" : "ies") + " carry usable active power evidence" +
                        (recovered > 0 ? "; " + std::to_string(recovered) +
                                             " of them were read back from durable storage and are therefore not "
                                             "the current state of the plant"
                                       : ""));
  } else if (missing == 0) {
    report.state = EvidenceState::Stale;
    explanation.add(ReasonCode::StaleEvidence, scope_label,
                    std::to_string(stale) + " entit" + (stale == 1 ? "y" : "ies") +
                        " carry evidence that is present but no longer usable");
  } else {
    report.state = EvidenceState::Indeterminate;
    explanation.add(ReasonCode::MissingMeasurement, scope_label,
                    std::to_string(missing) + " entit" + (missing == 1 ? "y" : "ies") +
                        " in scope carry no usable active power evidence");
  }

  explanation.canonicalize();
  report.explanation = explanation;
  return Outcome<FlowReport>(std::move(report), std::move(explanation));
}

}  // namespace po
