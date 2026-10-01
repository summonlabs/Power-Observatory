// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <string>
#include <string_view>

#include "power_observatory/divergence.hpp"
#include "power_observatory/json.hpp"
#include "power_observatory/persistence.hpp"
#include "power_observatory/runtime.hpp"
#include "power_observatory/snapshot.hpp"

namespace po {

// Every serializer emits objects with sorted keys and integers only, so the
// same answer always produces byte-identical text. Quantity fields carry their
// unit in the field name; the value is the exact decimal spelling produced by
// the quantity itself, never a rounded float.
[[nodiscard]] JsonValue to_json(const EvidenceRef& reference);
[[nodiscard]] JsonValue to_json(const Provenance& provenance);
[[nodiscard]] JsonValue to_json(const Measurement& measurement);
[[nodiscard]] JsonValue to_json(const EvidenceNote& note);
[[nodiscard]] JsonValue to_json(const Explanation& explanation);
[[nodiscard]] JsonValue to_json(const FlowReport& report);
[[nodiscard]] JsonValue to_json(const ReserveReport& report);
[[nodiscard]] JsonValue to_json(const AttributionReport& report);
[[nodiscard]] JsonValue to_json(const QualityReport& report);
[[nodiscard]] JsonValue to_json(const FailoverReport& report);
[[nodiscard]] JsonValue to_json(const AnswerReport& report);
[[nodiscard]] JsonValue to_json(const SnapshotMetadata& metadata);
[[nodiscard]] JsonValue to_json(const RecoveryReport& report);
[[nodiscard]] JsonValue to_json(const DivergenceReport& report);
[[nodiscard]] JsonValue to_json(const IngestResult& result);
[[nodiscard]] JsonValue to_json(const LoadEstimate& estimate);

// Parses an ingest document. Every field is validated; a wrong type, an
// unknown enumerator, or an out-of-range value is a refusal, never a default.
// Three mutually unambiguous entry points: a document, a string, and a string
// literal. Without the concrete string overloads a std::string argument would
// be ambiguous between the JsonValue conversion and the std::string_view
// conversion, and a string literal would be ambiguous for the same reason.
[[nodiscard]] Result<EvidenceBatch> parse_ingest_batch(const JsonValue& document);
[[nodiscard]] Result<EvidenceBatch> parse_ingest_batch(const std::string& text);
[[nodiscard]] Result<EvidenceBatch> parse_ingest_batch(std::string_view text);
[[nodiscard]] Result<EvidenceBatch> parse_ingest_batch(const char* text);

[[nodiscard]] JsonValue measurement_value_json(const MeasurementValue& value);

// Stamps a parsed batch with the delivery instant observed by this process.
//
// The document's own received_time is preserved, and the monotonic anchor is
// set back by the same amount, so the age this runtime measures equals the age
// the document declares. This is what makes live-ingested evidence eligible for
// a fresh classification; a batch that is never stamped stays anchorless and
// can never be reported as current.
void stamp_delivery(EvidenceBatch& batch, Timestamp now_wall, MonotonicInstant now_steady) noexcept;

}  // namespace po
