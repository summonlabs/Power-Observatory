// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/json_reports.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace po {
namespace {

template <class T>
[[nodiscard]] JsonValue optional_quantity(const std::optional<T>& value) {
  if (!value.has_value()) {
    return JsonValue(nullptr);
  }
  return JsonValue(value->canonical_value_string());
}

[[nodiscard]] JsonValue optional_ppm(const std::optional<Ratio>& value) {
  if (!value.has_value()) {
    return JsonValue(nullptr);
  }
  return JsonValue(value->raw());
}

[[nodiscard]] JsonValue optional_count(const std::optional<QuantityRep>& value) {
  if (!value.has_value()) {
    return JsonValue(nullptr);
  }
  return JsonValue(*value);
}

[[nodiscard]] JsonValue references_json(const std::vector<EvidenceRef>& references) {
  JsonValue array = JsonValue::array();
  for (const EvidenceRef& reference : references) {
    array.push(to_json(reference));
  }
  return array;
}

[[nodiscard]] JsonValue entities_json(const std::vector<EntityRef>& entities) {
  JsonValue array = JsonValue::array();
  for (const EntityRef& entity : entities) {
    array.push(JsonValue(entity.to_string()));
  }
  return array;
}

[[nodiscard]] JsonValue reasons_json(const Explanation& explanation) {
  JsonValue array = JsonValue::array();
  for (const Reason& reason : explanation.reasons()) {
    JsonValue entry = JsonValue::object();
    entry.set("code", JsonValue(std::string(po::to_string(reason.code))));
    entry.set("severity", JsonValue(std::string(po::to_string(reason.severity))));
    entry.set("category", JsonValue(std::string(po::to_string(category(reason.code)))));
    entry.set("subject", JsonValue(reason.subject));
    entry.set("detail", JsonValue(reason.detail));
    entry.set("evidence", references_json(reason.evidence));
    array.push(std::move(entry));
  }
  return array;
}

[[nodiscard]] Result<std::string> require_string(const JsonValue& document, std::string_view key) {
  const JsonValue* value = document.find(key);
  if (value == nullptr) {
    return Error(ReasonCode::SchemaViolation, "ingest document is missing the '" + std::string(key) + "' field");
  }
  if (value->type() != JsonValue::Type::String) {
    return Error(ReasonCode::SchemaViolation,
                 "ingest field '" + std::string(key) + "' must be a string");
  }
  return value->as_string();
}

[[nodiscard]] Result<std::uint64_t> require_unsigned(const JsonValue& document, std::string_view key) {
  const JsonValue* value = document.find(key);
  if (value == nullptr) {
    return Error(ReasonCode::SchemaViolation, "ingest document is missing the '" + std::string(key) + "' field");
  }
  if (value->type() == JsonValue::Type::Unsigned) {
    return value->as_unsigned();
  }
  if (value->type() == JsonValue::Type::Integer && value->as_integer() >= 0) {
    return static_cast<std::uint64_t>(value->as_integer());
  }
  return Error(ReasonCode::SchemaViolation,
               "ingest field '" + std::string(key) + "' must be a non-negative integer");
}

[[nodiscard]] Result<std::uint64_t> optional_unsigned(const JsonValue& document, std::string_view key,
                                                      std::uint64_t fallback) {
  if (!document.contains(key)) {
    return fallback;
  }
  return require_unsigned(document, key);
}

[[nodiscard]] Result<AuthorityKind> parse_authority(std::string_view text) {
  static constexpr std::array<std::pair<std::string_view, AuthorityKind>, 7> kTable = {{
      {"unknown", AuthorityKind::Unknown},
      {"observed", AuthorityKind::Observed},
      {"derived", AuthorityKind::Derived},
      {"configured", AuthorityKind::Configured},
      {"acknowledged", AuthorityKind::Acknowledged},
      {"external", AuthorityKind::External},
      {"synthetic", AuthorityKind::Synthetic},
  }};
  for (const auto& entry : kTable) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return Error(ReasonCode::SchemaViolation, "unrecognized authority '" + std::string(text) + "'");
}

[[nodiscard]] Result<EntityKind> parse_entity_kind(std::string_view text) {
  static constexpr std::array<std::pair<std::string_view, EntityKind>, 8> kTable = {{
      {"feed", EntityKind::Feed},
      {"bus", EntityKind::Bus},
      {"ups", EntityKind::Ups},
      {"generator", EntityKind::Generator},
      {"pdu", EntityKind::Pdu},
      {"circuit", EntityKind::Circuit},
      {"load", EntityKind::Load},
      {"redundancy_group", EntityKind::RedundancyGroup},
  }};
  for (const auto& entry : kTable) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return Error(ReasonCode::SchemaViolation, "unrecognized entity kind '" + std::string(text) + "'");
}

[[nodiscard]] Result<Phase> parse_phase(std::string_view text) {
  static constexpr std::array<std::pair<std::string_view, Phase>, 9> kTable = {{
      {"total", Phase::Total}, {"a", Phase::A},     {"b", Phase::B},      {"c", Phase::C},
      {"ab", Phase::AB},       {"bc", Phase::BC},   {"ca", Phase::CA},    {"neutral", Phase::Neutral},
      {"unknown", Phase::Unknown},
  }};
  for (const auto& entry : kTable) {
    if (entry.first == text) {
      return entry.second;
    }
  }
  return Error(ReasonCode::SchemaViolation, "unrecognized phase '" + std::string(text) + "'");
}

[[nodiscard]] Result<MeasurementValue> parse_measurement_value(std::string_view kind, std::string_view text) {
  if (kind == "active_power") {
    const Result<Power> value = parse_power(text);
    if (!value) {
      return value.error();
    }
    return MeasurementValue{value.value()};
  }
  if (kind == "apparent_power") {
    const Result<Power> value = parse_power(text);
    if (!value) {
      return value.error();
    }
    return MeasurementValue{ApparentPower::from_raw(value.value().raw())};
  }
  if (kind == "reactive_power") {
    const Result<Power> value = parse_power(text);
    if (!value) {
      return value.error();
    }
    return MeasurementValue{ReactivePower::from_raw(value.value().raw())};
  }
  if (kind == "voltage") {
    const Result<Voltage> value = parse_voltage(text);
    if (!value) {
      return value.error();
    }
    return MeasurementValue{value.value()};
  }
  if (kind == "current") {
    const Result<Current> value = parse_current(text);
    if (!value) {
      return value.error();
    }
    return MeasurementValue{value.value()};
  }
  if (kind == "frequency") {
    const Result<Frequency> value = parse_frequency(text);
    if (!value) {
      return value.error();
    }
    return MeasurementValue{value.value()};
  }
  if (kind == "power_factor") {
    const Result<Ratio> value = parse_ratio_ppm(text);
    if (!value) {
      return value.error();
    }
    return MeasurementValue{value.value()};
  }
  if (kind == "energy") {
    const Result<QuantityRep> value = parse_decimal_scaled(text, 1000, "energy");
    if (!value) {
      return value.error();
    }
    return MeasurementValue{Energy::from_raw(value.value())};
  }
  if (kind == "temperature") {
    const Result<QuantityRep> value = parse_decimal_scaled(text, 1000, "temperature");
    if (!value) {
      return value.error();
    }
    return MeasurementValue{Temperature::from_raw(value.value())};
  }
  return Error(ReasonCode::SchemaViolation, "unrecognized measurement kind '" + std::string(kind) + "'");
}

}  // namespace

JsonValue measurement_value_json(const MeasurementValue& value) {
  JsonValue object = JsonValue::object();
  object.set("kind", JsonValue(std::string(po::to_string(kind_of(value)))));
  object.set("raw", JsonValue(raw_value(value)));
  std::visit(
      [&object](const auto& typed) {
        using T = std::decay_t<decltype(typed)>;
        object.set("unit", JsonValue(std::string(T::unit_tag::canonical_unit)));
        object.set("value", JsonValue(typed.canonical_value_string()));
      },
      value);
  return object;
}

JsonValue to_json(const EvidenceRef& reference) {
  JsonValue object = JsonValue::object();
  object.set("entity", JsonValue(reference.entity.to_string()));
  object.set("generation", JsonValue(reference.generation.value()));
  object.set("kind", JsonValue(std::string(po::to_string(reference.kind))));
  object.set("measurement", JsonValue(reference.measurement.value()));
  object.set("phase", JsonValue(std::string(po::to_string(reference.phase))));
  object.set("received_time", JsonValue(reference.received_time.to_iso8601()));
  object.set("source", JsonValue(reference.source.value()));
  return object;
}

JsonValue to_json(const Provenance& provenance) {
  JsonValue object = JsonValue::object();
  object.set("authority", JsonValue(std::string(po::to_string(provenance.authority))));
  object.set("epoch", JsonValue(provenance.epoch.value()));
  object.set("generation", JsonValue(provenance.generation.value()));
  object.set("has_monotonic_anchor", JsonValue(provenance.has_monotonic_anchor));
  object.set("origin", JsonValue(std::string(po::to_string(provenance.origin))));
  object.set("recovered", JsonValue(provenance.recovered()));
  object.set("received_time", JsonValue(provenance.received_time.to_iso8601()));
  object.set("sequence", JsonValue(provenance.sequence.value()));
  object.set("source", JsonValue(provenance.source.value()));
  object.set("source_time", provenance.source_time.has_value()
                                ? JsonValue(provenance.source_time->to_iso8601())
                                : JsonValue(nullptr));
  return object;
}

JsonValue to_json(const Measurement& measurement) {
  JsonValue object = JsonValue::object();
  object.set("entity", JsonValue(measurement.entity.to_string()));
  object.set("id", JsonValue(measurement.id.value()));
  object.set("phase", JsonValue(std::string(po::to_string(measurement.phase))));
  object.set("provenance", to_json(measurement.provenance));
  object.set("value", measurement_value_json(measurement.value));
  return object;
}

JsonValue to_json(const EvidenceNote& note) {
  JsonValue object = JsonValue::object();
  object.set("code", JsonValue(std::string(po::to_string(note.code))));
  object.set("detail", JsonValue(note.detail));
  object.set("entities", entities_json(note.entities));
  object.set("severity", JsonValue(std::string(po::to_string(note.severity))));
  return object;
}

JsonValue to_json(const Explanation& explanation) {
  JsonValue object = JsonValue::object();
  object.set("content_hash", JsonValue(explanation.content_hash()));
  JsonValue dependencies = JsonValue::array();
  for (const EvidenceDependency& dependency : explanation.dependencies()) {
    JsonValue entry = JsonValue::object();
    entry.set("generation", JsonValue(dependency.generation.value()));
    entry.set("measurement_count", JsonValue(static_cast<std::uint64_t>(dependency.measurement_count)));
    entry.set("source", JsonValue(dependency.source.value()));
    dependencies.push(std::move(entry));
  }
  object.set("dependencies", std::move(dependencies));
  object.set("evidence", references_json(explanation.evidence()));
  object.set("has_failure", JsonValue(explanation.has_failure()));
  object.set("reasons", reasons_json(explanation));
  object.set("worst_severity", JsonValue(std::string(po::to_string(explanation.worst_severity()))));
  return object;
}

JsonValue to_json(const FlowReport& report) {
  JsonValue object = JsonValue::object();
  object.set("computed_at", JsonValue(report.computed_at.to_iso8601()));
  object.set("explanation", to_json(report.explanation));
  object.set("measured_nodes", JsonValue(static_cast<std::uint64_t>(report.measured_nodes)));
  JsonValue nodes = JsonValue::array();
  for (const FlowNode& node : report.nodes) {
    JsonValue entry = JsonValue::object();
    entry.set("active_power_w", optional_quantity(node.active_power));
    entry.set("child_residual_ppm", optional_ppm(node.child_residual_ppm));
    entry.set("child_residual_w", optional_quantity(node.child_residual));
    entry.set("counted", JsonValue(node.counted));
    entry.set("direction", JsonValue(std::string(po::to_string(node.direction))));
    entry.set("entity", JsonValue(node.entity.to_string()));
    entry.set("evidence", references_json(node.evidence));
    entry.set("freshness", JsonValue(std::string(po::to_string(node.freshness))));
    entry.set("measured_children", entities_json(node.measured_children));
    entry.set("share_of_total_ppm", optional_ppm(node.share_of_total_ppm));
    entry.set("state", JsonValue(std::string(po::to_string(node.state))));
    entry.set("unmeasured_children", entities_json(node.unmeasured_children));
    nodes.push(std::move(entry));
  }
  object.set("nodes", std::move(nodes));
  object.set("state", JsonValue(std::string(po::to_string(report.state))));
  object.set("total_observed_load_w", optional_quantity(report.total_observed_load));
  object.set("unmeasured_nodes", JsonValue(static_cast<std::uint64_t>(report.unmeasured_nodes)));
  return object;
}

JsonValue to_json(const ReserveReport& report) {
  JsonValue object = JsonValue::object();
  object.set("below_minimum_headroom", JsonValue(report.below_minimum_headroom));
  JsonValue components = JsonValue::array();
  for (const ReserveComponent& component : report.components) {
    JsonValue entry = JsonValue::object();
    entry.set("declared_capacity_w", JsonValue(component.declared_capacity.canonical_value_string()));
    entry.set("declared_state", JsonValue(std::string(po::to_string(component.declared_state))));
    entry.set("entity", JsonValue(component.entity.to_string()));
    entry.set("evidence", references_json(component.evidence));
    entry.set("load_fraction_ppm", optional_ppm(component.load_fraction_ppm));
    entry.set("measured_load_w", optional_quantity(component.measured_load));
    entry.set("reserve_w", optional_quantity(component.reserve));
    entry.set("state", JsonValue(std::string(po::to_string(component.state))));
    entry.set("usable_capacity_w", optional_quantity(component.usable_capacity));
    components.push(std::move(entry));
  }
  object.set("components", std::move(components));
  object.set("downstream_measured_load_w", optional_quantity(report.downstream_measured_load));
  object.set("explanation", to_json(report.explanation));
  object.set("known_components", JsonValue(static_cast<std::uint64_t>(report.known_components)));
  object.set("measured_load_w", optional_quantity(report.measured_load));
  object.set("reserve_fraction_ppm", optional_ppm(report.reserve_fraction_ppm));
  object.set("reserve_w", optional_quantity(report.reserve));
  object.set("scope", JsonValue(report.scope.to_string()));
  object.set("single_failure_capable", JsonValue(report.single_failure_capable));
  object.set("single_failure_reserve_w", optional_quantity(report.single_failure_reserve));
  object.set("state", JsonValue(std::string(po::to_string(report.state))));
  object.set("supply_downstream_delta_ppm",
             optional_ppm(report.supply_downstream_delta_ppm));
  object.set("total_components", JsonValue(static_cast<std::uint64_t>(report.total_components)));
  object.set("usable_capacity_w", optional_quantity(report.usable_capacity));
  return object;
}

JsonValue to_json(const AttributionReport& report) {
  JsonValue object = JsonValue::object();
  object.set("computed_at", JsonValue(report.computed_at.to_iso8601()));
  object.set("explanation", to_json(report.explanation));
  JsonValue imbalances = JsonValue::array();
  for (const ImbalanceRecord& record : report.imbalances) {
    JsonValue entry = JsonValue::object();
    entry.set("children_total_w", JsonValue(record.children_total.canonical_value_string()));
    entry.set("evidence", references_json(record.evidence));
    entry.set("measured_children", entities_json(record.measured_children));
    entry.set("parent", JsonValue(record.parent.to_string()));
    entry.set("parent_measured_w", JsonValue(record.parent_measured.canonical_value_string()));
    entry.set("residual_ppm", optional_ppm(record.residual_ppm));
    entry.set("residual_w", JsonValue(record.residual.canonical_value_string()));
    entry.set("state", JsonValue(std::string(po::to_string(record.state))));
    entry.set("tolerance_ppm", JsonValue(record.tolerance_ppm.raw()));
    entry.set("unmeasured_children", entities_json(record.unmeasured_children));
    entry.set("within_tolerance", JsonValue(record.within_tolerance));
    imbalances.push(std::move(entry));
  }
  object.set("imbalances", std::move(imbalances));
  JsonValue losses = JsonValue::array();
  for (const LossRecord& record : report.losses) {
    JsonValue entry = JsonValue::object();
    entry.set("declared_efficiency_ppm", optional_ppm(record.declared_efficiency_ppm));
    entry.set("evidence", references_json(record.evidence));
    entry.set("input_w", JsonValue(record.input.canonical_value_string()));
    entry.set("limit_ppm", JsonValue(record.limit_ppm.raw()));
    entry.set("loss_ppm", optional_ppm(record.loss_ppm));
    entry.set("loss_w", JsonValue(record.loss.canonical_value_string()));
    entry.set("node", JsonValue(record.node.to_string()));
    entry.set("output_w", JsonValue(record.output.canonical_value_string()));
    entry.set("state", JsonValue(std::string(po::to_string(record.state))));
    entry.set("within_limit", JsonValue(record.within_limit));
    losses.push(std::move(entry));
  }
  object.set("losses", std::move(losses));
  object.set("state", JsonValue(std::string(po::to_string(report.state))));
  object.set("total_measured_loss_w", optional_quantity(report.total_measured_loss));
  object.set("unattributed_imbalances", JsonValue(static_cast<std::uint64_t>(report.unattributed_imbalances)));
  return object;
}

JsonValue to_json(const QualityReport& report) {
  JsonValue object = JsonValue::object();
  object.set("computed_at", JsonValue(report.computed_at.to_iso8601()));
  object.set("explanation", to_json(report.explanation));
  JsonValue findings = JsonValue::array();
  for (const QualityFinding& finding : report.findings) {
    JsonValue entry = JsonValue::object();
    entry.set("code", JsonValue(std::string(po::to_string(finding.code))));
    entry.set("detail", JsonValue(finding.detail));
    entry.set("entity", JsonValue(finding.entity.to_string()));
    entry.set("evidence", finding.evidence.has_value() ? to_json(*finding.evidence) : JsonValue(nullptr));
    entry.set("kind", JsonValue(std::string(po::to_string(finding.kind))));
    entry.set("phase", JsonValue(std::string(po::to_string(finding.phase))));
    entry.set("severity", JsonValue(std::string(po::to_string(finding.severity))));
    findings.push(std::move(entry));
  }
  object.set("findings", std::move(findings));
  object.set("measurements_assessed", JsonValue(static_cast<std::uint64_t>(report.measurements_assessed)));
  object.set("measurements_skipped_stale", JsonValue(static_cast<std::uint64_t>(report.measurements_skipped_stale)));
  object.set("state", JsonValue(std::string(po::to_string(report.state))));
  return object;
}

JsonValue to_json(const FailoverReport& report) {
  JsonValue object = JsonValue::object();
  object.set("currently_live", JsonValue(static_cast<std::uint64_t>(report.currently_live)));
  object.set("explanation", to_json(report.explanation));
  JsonValue gates = JsonValue::array();
  for (const FailoverGate& gate : report.gates) {
    JsonValue entry = JsonValue::object();
    entry.set("code", JsonValue(std::string(po::to_string(gate.code))));
    entry.set("detail", JsonValue(gate.detail));
    entry.set("evaluable", JsonValue(gate.evaluable));
    entry.set("evidence", references_json(gate.evidence));
    entry.set("gate", JsonValue(gate.gate));
    entry.set("mandatory", JsonValue(gate.mandatory));
    entry.set("passed", JsonValue(gate.passed));
    gates.push(std::move(entry));
  }
  object.set("gates", std::move(gates));
  object.set("group", JsonValue(report.group.value()));
  object.set("load_to_transfer_w", optional_quantity(report.load_to_transfer));
  object.set("peer_headroom_after_single_failure_w", optional_quantity(report.peer_headroom_after_single_failure));
  object.set("readiness", JsonValue(std::string(po::to_string(report.readiness))));
  object.set("required_live", JsonValue(static_cast<std::uint64_t>(report.required_live)));
  object.set("topology", JsonValue(std::string(po::to_string(report.topology))));
  return object;
}

JsonValue to_json(const AnswerReport& report) {
  JsonValue object = JsonValue::object();
  object.set("computed_at", JsonValue(report.computed_at.to_iso8601()));
  object.set("explanation", to_json(report.explanation));
  object.set("failover_degraded", JsonValue(static_cast<std::uint64_t>(report.failover_degraded)));
  object.set("failover_not_ready", JsonValue(static_cast<std::uint64_t>(report.failover_not_ready)));
  object.set("failover_ready", JsonValue(static_cast<std::uint64_t>(report.failover_ready)));
  object.set("failover_unknown", JsonValue(static_cast<std::uint64_t>(report.failover_unknown)));
  JsonValue groups = JsonValue::array();
  for (const FailoverReport& group : report.groups) {
    groups.push(to_json(group));
  }
  object.set("groups", std::move(groups));
  object.set("measured_entities", JsonValue(static_cast<std::uint64_t>(report.measured_entities)));
  object.set("quality_findings", JsonValue(static_cast<std::uint64_t>(report.quality_findings)));
  object.set("revision", JsonValue(report.revision.value()));
  object.set("state", JsonValue(std::string(po::to_string(report.state))));
  object.set("total_observed_load_w", optional_quantity(report.total_observed_load));
  object.set("total_reserve_w", optional_quantity(report.total_reserve));
  object.set("total_usable_capacity_w", optional_quantity(report.total_usable_capacity));
  object.set("unattributed_imbalances", JsonValue(static_cast<std::uint64_t>(report.unattributed_imbalances)));
  object.set("unmeasured_entities", JsonValue(static_cast<std::uint64_t>(report.unmeasured_entities)));
  return object;
}

JsonValue to_json(const SnapshotMetadata& metadata) {
  JsonValue object = JsonValue::object();
  object.set("computed_at", JsonValue(metadata.computed_at.to_iso8601()));
  object.set("dropped_measurement_count", JsonValue(static_cast<std::uint64_t>(metadata.dropped_measurement_count)));
  object.set("epoch", JsonValue(metadata.epoch.value()));
  object.set("evidence_hash", JsonValue(metadata.evidence_hash));
  object.set("generations", [&metadata] {
    JsonValue array = JsonValue::array();
    for (const Generation& generation : metadata.generations) {
      array.push(JsonValue(generation.value()));
    }
    return array;
  }());
  object.set("measurement_count", JsonValue(static_cast<std::uint64_t>(metadata.measurement_count)));
  object.set("policy_hash", JsonValue(metadata.policy_hash));
  object.set("revision", JsonValue(metadata.revision.value()));
  object.set("topology_hash", JsonValue(metadata.topology_hash));
  return object;
}

JsonValue to_json(const RecoveryReport& report) {
  JsonValue object = JsonValue::object();
  object.set("bytes_discarded", JsonValue(report.bytes_discarded));
  object.set("committed_length", JsonValue(report.committed_length));
  object.set("created_new", JsonValue(report.created_new));
  object.set("epoch", JsonValue(report.epoch.value()));
  object.set("header_valid", JsonValue(report.header_valid));
  object.set("highest_generation", JsonValue(report.highest_generation.value()));
  JsonValue notes = JsonValue::array();
  for (const EvidenceNote& note : report.notes) {
    notes.push(to_json(note));
  }
  object.set("notes", std::move(notes));
  object.set("records_applied", JsonValue(static_cast<std::uint64_t>(report.records_applied)));
  object.set("records_rejected", JsonValue(static_cast<std::uint64_t>(report.records_rejected)));
  object.set("records_scanned", JsonValue(static_cast<std::uint64_t>(report.records_scanned)));
  object.set("records_skipped_replay", JsonValue(static_cast<std::uint64_t>(report.records_skipped_replay)));
  object.set("tail_detail", JsonValue(report.tail_detail));
  object.set("tail_reason", JsonValue(std::string(po::to_string(report.tail_reason))));
  object.set("truncated", JsonValue(report.truncated));
  return object;
}

JsonValue to_json(const DivergenceReport& report) {
  JsonValue object = JsonValue::object();
  object.set("appeared", JsonValue(static_cast<std::uint64_t>(report.appeared)));
  object.set("compared_keys", JsonValue(static_cast<std::uint64_t>(report.compared_keys)));
  object.set("disappeared", JsonValue(static_cast<std::uint64_t>(report.disappeared)));
  object.set("diverged_keys", JsonValue(static_cast<std::uint64_t>(report.diverged_keys)));
  JsonValue entries = JsonValue::array();
  for (const DivergenceEntry& entry : report.entries) {
    JsonValue item = JsonValue::object();
    item.set("classification", JsonValue(std::string(po::to_string(entry.classification))));
    item.set("detail", JsonValue(entry.detail));
    item.set("entity", JsonValue(entry.entity.to_string()));
    item.set("kind", JsonValue(std::string(po::to_string(entry.kind))));
    item.set("left_generation", entry.left_generation.has_value() ? JsonValue(entry.left_generation->value())
                                                                  : JsonValue(nullptr));
    item.set("left_raw", optional_count(entry.left_raw));
    item.set("phase", JsonValue(std::string(po::to_string(entry.phase))));
    item.set("right_generation", entry.right_generation.has_value() ? JsonValue(entry.right_generation->value())
                                                                    : JsonValue(nullptr));
    item.set("right_raw", optional_count(entry.right_raw));
    entries.push(std::move(item));
  }
  object.set("entries", std::move(entries));
  object.set("explanation", to_json(report.explanation));
  object.set("generation_changes", JsonValue(static_cast<std::uint64_t>(report.generation_changes)));
  object.set("order", JsonValue(std::string(po::to_string(report.order))));
  object.set("value_changes", JsonValue(static_cast<std::uint64_t>(report.value_changes)));
  object.set("verdict", JsonValue(std::string(po::to_string(report.verdict))));
  return object;
}

JsonValue to_json(const IngestResult& result) {
  JsonValue object = JsonValue::object();
  object.set("code", JsonValue(std::string(po::to_string(result.code))));
  object.set("detail", JsonValue(result.detail));
  object.set("disposition", JsonValue(std::string(po::to_string(result.disposition))));
  object.set("published_revision", JsonValue(result.published_revision.value()));
  object.set("record", JsonValue(result.record.value()));
  return object;
}

JsonValue to_json(const LoadEstimate& estimate) {
  JsonValue object = JsonValue::object();
  object.set("complete", JsonValue(estimate.complete()));
  object.set("counted", entities_json(estimate.counted));
  object.set("evidence", references_json(estimate.evidence));
  object.set("scope", JsonValue(estimate.scope.to_string()));
  object.set("total_w", optional_quantity(estimate.total));
  object.set("unmeasured", entities_json(estimate.unmeasured));
  return object;
}

void stamp_delivery(EvidenceBatch& batch, Timestamp now_wall, MonotonicInstant now_steady) noexcept {
  for (Measurement& measurement : batch.measurements) {
    measurement.provenance.origin = EvidenceOrigin::LiveIngest;
    measurement.provenance.has_monotonic_anchor = true;
    measurement.provenance.received_steady = now_steady;

    const Result<Duration> declared_age = sub(now_wall, measurement.provenance.received_time);
    if (!declared_age || declared_age.value().raw() <= 0) {
      continue;
    }
    QuantityRep lookback{};
    if (!checked_neg(declared_age.value().raw(), lookback)) {
      continue;
    }
    const Result<MonotonicInstant> anchor = add(now_steady, Duration::from_raw(lookback));
    if (anchor) {
      measurement.provenance.received_steady = anchor.value();
    }
  }
}

Result<EvidenceBatch> parse_ingest_batch(const JsonValue& document) {
  if (document.type() != JsonValue::Type::Object) {
    return Error(ReasonCode::SchemaViolation, "ingest document must be a JSON object");
  }

  EvidenceBatch batch;

  const Result<std::string> source = require_string(document, "source");
  if (!source) {
    return source.error();
  }
  if (source.value().empty()) {
    return Error(ReasonCode::SchemaViolation, "ingest document declares an empty source identifier");
  }
  batch.source = SourceId(source.value());

  const Result<std::string> authority_text = require_string(document, "authority");
  if (!authority_text) {
    return authority_text.error();
  }
  const Result<AuthorityKind> authority = parse_authority(authority_text.value());
  if (!authority) {
    return authority.error();
  }
  batch.authority = authority.value();
  if (!carries_measurement(batch.authority) && batch.authority != AuthorityKind::Synthetic) {
    return Error(ReasonCode::AuthorityViolation,
                 "authority '" + std::string(po::to_string(batch.authority)) +
                     "' may not carry measurements; a controller acknowledgement or a configuration statement is "
                     "not observed electrical state");
  }

  const Result<std::uint64_t> generation = optional_unsigned(document, "generation", 1);
  if (!generation) {
    return generation.error();
  }
  batch.generation = Generation(generation.value());

  const Result<std::uint64_t> epoch = optional_unsigned(document, "epoch", 0);
  if (!epoch) {
    return epoch.error();
  }
  batch.epoch = Epoch(epoch.value());

  const Result<std::uint64_t> sequence = optional_unsigned(document, "first_sequence", 1);
  if (!sequence) {
    return sequence.error();
  }
  batch.first_sequence = Sequence(sequence.value());

  const Result<std::uint64_t> mutation = optional_unsigned(document, "mutation", sequence.value());
  if (!mutation) {
    return mutation.error();
  }
  batch.mutation = MutationId(mutation.value());

  const Result<std::uint64_t> attempt = optional_unsigned(document, "attempt", 1);
  if (!attempt) {
    return attempt.error();
  }
  batch.attempt = AttemptId(attempt.value());

  const JsonValue* recorded = document.find("recorded_at");
  if (recorded != nullptr) {
    if (recorded->type() != JsonValue::Type::String) {
      return Error(ReasonCode::SchemaViolation, "ingest field 'recorded_at' must be a timestamp string");
    }
    const Result<Timestamp> parsed = Timestamp::from_iso8601(recorded->as_string());
    if (!parsed) {
      return parsed.error();
    }
    batch.recorded_at = parsed.value();
  }

  const JsonValue* measurements = document.find("measurements");
  if (measurements == nullptr || measurements->type() != JsonValue::Type::Array) {
    return Error(ReasonCode::SchemaViolation, "ingest document must carry a 'measurements' array");
  }

  for (const JsonValue& entry : measurements->as_array()) {
    if (entry.type() != JsonValue::Type::Object) {
      return Error(ReasonCode::SchemaViolation, "each measurement must be a JSON object");
    }
    const Result<std::uint64_t> id = require_unsigned(entry, "id");
    if (!id) {
      return id.error();
    }
    const Result<std::string> entity_text = require_string(entry, "entity");
    if (!entity_text) {
      return entity_text.error();
    }
    const Result<EntityRef> entity = EntityRef::parse(entity_text.value());
    if (!entity) {
      return entity.error();
    }

    Phase phase = Phase::Total;
    if (entry.contains("phase")) {
      const Result<std::string> phase_text = require_string(entry, "phase");
      if (!phase_text) {
        return phase_text.error();
      }
      const Result<Phase> parsed = parse_phase(phase_text.value());
      if (!parsed) {
        return parsed.error();
      }
      phase = parsed.value();
    }

    const Result<std::string> kind_text = require_string(entry, "kind");
    if (!kind_text) {
      return kind_text.error();
    }
    const JsonValue* value_node = entry.find("value");
    if (value_node == nullptr || value_node->type() != JsonValue::Type::String) {
      return Error(ReasonCode::SchemaViolation, "each measurement must carry a string 'value' field");
    }
    const Result<MeasurementValue> value = parse_measurement_value(kind_text.value(), value_node->as_string());
    if (!value) {
      return value.error();
    }

    Measurement measurement;
    measurement.id = MeasurementId(id.value());
    measurement.entity = entity.value();
    measurement.phase = phase;
    measurement.value = value.value();

    std::optional<Timestamp> source_time;
    if (entry.contains("source_time")) {
      const JsonValue* node = entry.find("source_time");
      if (node->type() != JsonValue::Type::Null) {
        if (node->type() != JsonValue::Type::String) {
          return Error(ReasonCode::SchemaViolation, "measurement 'source_time' must be a timestamp string or null");
        }
        const Result<Timestamp> parsed = Timestamp::from_iso8601(node->as_string());
        if (!parsed) {
          return parsed.error();
        }
        source_time = parsed.value();
      }
    }

    std::uint64_t measurement_generation = batch.generation.value();
    if (entry.contains("generation")) {
      const Result<std::uint64_t> parsed = require_unsigned(entry, "generation");
      if (!parsed) {
        return parsed.error();
      }
      measurement_generation = parsed.value();
    }

    std::uint64_t measurement_sequence = batch.first_sequence.value();
    if (entry.contains("sequence")) {
      const Result<std::uint64_t> parsed = require_unsigned(entry, "sequence");
      if (!parsed) {
        return parsed.error();
      }
      measurement_sequence = parsed.value();
    }

    Timestamp received = batch.recorded_at;
    if (entry.contains("received_time")) {
      const Result<std::string> text = require_string(entry, "received_time");
      if (!text) {
        return text.error();
      }
      const Result<Timestamp> parsed = Timestamp::from_iso8601(text.value());
      if (!parsed) {
        return parsed.error();
      }
      received = parsed.value();
    }

    // Ingested evidence is observed by this process, so it carries a live
    // delivery anchor. Evidence read back from the log is rebuilt through
    // make_recovered_provenance instead, which is what keeps recovered evidence
    // from ever being classified as fresh.
    measurement.provenance = make_observed_provenance(
        batch.source, Generation(measurement_generation), batch.epoch, Sequence(measurement_sequence), source_time,
        batch.authority, received, MonotonicInstant{});
    measurement.provenance.has_monotonic_anchor = false;

    batch.measurements.push_back(std::move(measurement));
  }

  if (batch.measurements.empty()) {
    return Error(ReasonCode::SchemaViolation, "ingest document carries no measurements");
  }
  return batch;
}

Result<EvidenceBatch> parse_ingest_batch(std::string_view text) {
  const Result<JsonValue> document = JsonValue::parse(text);
  if (!document) {
    return document.error();
  }
  return parse_ingest_batch(document.value());
}

Result<EvidenceBatch> parse_ingest_batch(const std::string& text) {
  return parse_ingest_batch(std::string_view(text));
}

Result<EvidenceBatch> parse_ingest_batch(const char* text) {
  return parse_ingest_batch(std::string_view(text == nullptr ? "" : text));
}

}  // namespace po