// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "power_observatory/json.hpp"
#include "power_observatory/json_reports.hpp"

using namespace po;

PO_TEST(json, object_keys_are_emitted_sorted) {
  JsonValue object = JsonValue::object();
  object.set("zeta", JsonValue(1));
  object.set("alpha", JsonValue(2));
  object.set("mu", JsonValue(3));
  PO_CHECK_EQ(object.dump(), std::string(R"({"alpha":2,"mu":3,"zeta":1})"));
}

PO_TEST(json, duplicate_key_replaces_in_place) {
  JsonValue object = JsonValue::object();
  object.set("a", JsonValue(1));
  object.set("b", JsonValue(2));
  object.set("a", JsonValue(3));
  PO_CHECK_EQ(object.dump(), std::string(R"({"a":3,"b":2})"));
}

PO_TEST(json, strings_escape_control_characters) {
  JsonValue value(std::string("line\nbreak\ttab\"quote\\slash"));
  PO_CHECK_EQ(value.dump(), std::string("\"line\\nbreak\\ttab\\\"quote\\\\slash\""));
}

PO_TEST(json, non_ascii_passes_through_unchanged) {
  JsonValue value(std::string("m\u00b5s \xe2\x84\xa6"));
  PO_CHECK_EQ(value.dump(), std::string("\"m\u00b5s \xe2\x84\xa6\""));
}

PO_TEST(json, parser_round_trips_canonical_output) {
  // The parser accepts any key order and the writer always emits sorted keys,
  // so a document that is already sorted survives a parse-and-dump unchanged.
  const std::string text = R"({"a":{"n":-17,"u":18446744073709551615},"b":[true,false,null]})";
  const Result<JsonValue> parsed = JsonValue::parse(text);
  PO_REQUIRE_OK(parsed);
  PO_CHECK_EQ(parsed.value().dump(), text);

  const Result<JsonValue> unsorted = JsonValue::parse(R"({"b":[true,false,null],"a":{"u":18446744073709551615,"n":-17}})");
  PO_REQUIRE_OK(unsorted);
  PO_CHECK_EQ(unsorted.value().dump(), text);
}

PO_TEST(json, parser_refuses_trailing_content) {
  PO_REQUIRE_ERR(JsonValue::parse(R"({"a":1} extra)"), ReasonCode::ParseError);
}

PO_TEST(json, parser_refuses_floating_point) {
  // This runtime carries every number as a 64-bit integer. Accepting 1.5 would
  // introduce a representation it cannot round-trip, so it is refused.
  PO_REQUIRE_ERR(JsonValue::parse("1.5"), ReasonCode::UnsupportedToken);
  PO_REQUIRE_ERR(JsonValue::parse("1e3"), ReasonCode::UnsupportedToken);
}

PO_TEST(json, parser_refuses_out_of_range_integers) {
  PO_REQUIRE_ERR(JsonValue::parse("18446744073709551616"), ReasonCode::ValueOutOfRange);
}

PO_TEST(json, parser_handles_surrogate_pairs_and_refuses_unpaired) {
  const Result<JsonValue> paired = JsonValue::parse(R"("\ud83d\ude00")");
  PO_REQUIRE_OK(paired);
  PO_CHECK_EQ(paired.value().as_string(), std::string("\xf0\x9f\x98\x80"));

  PO_REQUIRE_ERR(JsonValue::parse(R"("\ud83d")"), ReasonCode::ParseError);
  PO_REQUIRE_ERR(JsonValue::parse(R"("\ude00")"), ReasonCode::ParseError);
}

PO_TEST(json, parser_refuses_truncated_input) {
  PO_REQUIRE_ERR(JsonValue::parse(R"({"a":)"), ReasonCode::UnexpectedEndOfInput);
  PO_REQUIRE_ERR(JsonValue::parse(R"("unterminated)"), ReasonCode::UnexpectedEndOfInput);
  PO_REQUIRE_ERR(JsonValue::parse("[1,2"), ReasonCode::UnexpectedEndOfInput);
}

PO_TEST(json, deep_nesting_is_bounded) {
  std::string deep;
  for (int index = 0; index < 200; ++index) {
    deep.push_back('[');
  }
  PO_REQUIRE_ERR(JsonValue::parse(deep), ReasonCode::ParseError);
}

PO_TEST(json, ingest_document_rejects_a_non_measurement_authority) {
  const std::string text =
      R"({"source":"s","authority":"acknowledged","measurements":[{"id":1,"entity":"feed:a","kind":"active_power","value":"1 kW"}]})";
  PO_REQUIRE_ERR(parse_ingest_batch(text), ReasonCode::AuthorityViolation);
}

PO_TEST(json, ingest_document_rejects_unknown_enumerators) {
  const std::string text =
      R"({"source":"s","authority":"observed","measurements":[{"id":1,"entity":"widget:a","kind":"active_power","value":"1 kW"}]})";
  // An entity reference that names no known kind is a parse failure of the
  // reference itself, which is a different code from a schema violation of the
  // surrounding document.
  PO_REQUIRE_ERR(parse_ingest_batch(text), ReasonCode::ParseError);

  const std::string kind_text =
      R"({"source":"s","authority":"observed","measurements":[{"id":1,"entity":"feed:a","kind":"fluffiness","value":"1"}]})";
  PO_REQUIRE_ERR(parse_ingest_batch(kind_text), ReasonCode::SchemaViolation);
}

PO_TEST(json, ingest_document_requires_every_declared_field) {
  PO_REQUIRE_ERR(parse_ingest_batch(R"({"authority":"observed","measurements":[]})"), ReasonCode::SchemaViolation);
  PO_REQUIRE_ERR(parse_ingest_batch(R"({"source":"s","measurements":[]})"), ReasonCode::SchemaViolation);
  PO_REQUIRE_ERR(parse_ingest_batch(R"({"source":"s","authority":"observed"})"), ReasonCode::SchemaViolation);
  PO_REQUIRE_ERR(parse_ingest_batch(R"({"source":"s","authority":"observed","measurements":[]})"),
                 ReasonCode::SchemaViolation);
}

PO_TEST(json, ingest_document_admits_a_well_formed_batch) {
  const std::string text =
      R"({"source":"meter-1","authority":"observed","generation":4,"epoch":2,"first_sequence":10,"mutation":5,)"
      R"("recorded_at":"2026-01-01T00:00:00.000000000Z","measurements":[)"
      R"({"id":1,"entity":"feed:main-a","phase":"total","kind":"active_power","value":"1234.567 kW"},)"
      R"({"id":2,"entity":"feed:main-a","phase":"a","kind":"voltage","value":"230.1 V"}]})";
  const Result<EvidenceBatch> batch = parse_ingest_batch(text);
  PO_REQUIRE_OK(batch);
  PO_CHECK_EQ(batch.value().measurements.size(), std::size_t{2});
  PO_CHECK_EQ(batch.value().measurements[0].provenance.generation.value(), std::uint64_t{4});
  PO_CHECK_EQ(batch.value().measurements[0].provenance.authority, AuthorityKind::Observed);
  PO_CHECK_EQ(std::get<Power>(batch.value().measurements[0].value).raw(), QuantityRep{1234567000});
}

PO_TEST(json, ingest_document_refuses_more_precision_than_the_unit_carries) {
  const std::string text =
      R"({"source":"s","authority":"observed","measurements":[{"id":1,"entity":"feed:a","kind":"active_power","value":"0.0001 W"}]})";
  PO_REQUIRE_ERR(parse_ingest_batch(text), ReasonCode::ValueOutOfRange);
}

PO_TEST_MAIN()
