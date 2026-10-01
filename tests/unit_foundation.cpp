// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <limits>

#include "power_observatory/quantity.hpp"
#include "power_observatory/reason.hpp"
#include "power_observatory/result.hpp"
#include "power_observatory/time.hpp"
#include "power_observatory/version.hpp"

using namespace po;

PO_TEST(quantity, checked_add_detects_overflow) {
  QuantityRep out = 0;
  PO_CHECK(checked_add(1, 2, out));
  PO_CHECK_EQ(out, 3);
  PO_CHECK(!checked_add(std::numeric_limits<QuantityRep>::max(), 1, out));
  PO_CHECK(!checked_add(std::numeric_limits<QuantityRep>::min(), -1, out));
}

PO_TEST(quantity, checked_mul_matches_wide_reference) {
  QuantityRep out = 0;
  PO_CHECK(checked_mul(1000000, 1000000, out));
  PO_CHECK_EQ(out, 1000000000000);
  PO_CHECK(!checked_mul(std::numeric_limits<QuantityRep>::max(), 2, out));
  PO_CHECK(checked_mul(std::numeric_limits<QuantityRep>::min(), 1, out));
  PO_CHECK_EQ(out, std::numeric_limits<QuantityRep>::min());
}

PO_TEST(quantity, format_is_exact) {
  PO_CHECK_EQ((Power::from_raw(1234567).canonical_value_string()), std::string("1234.567"));
  PO_CHECK_EQ((Power::from_raw(-1500).canonical_value_string()), std::string("-1.5"));
  PO_CHECK_EQ((Power::from_raw(0).canonical_value_string()), std::string("0"));
  PO_CHECK_EQ((Power::from_raw(1000).to_string()), std::string("1 W"));
}

PO_TEST(quantity, parse_refuses_inexact_values) {
  const Result<Power> exact = parse_power("1234.567 W");
  PO_REQUIRE_OK(exact);
  PO_CHECK_EQ(exact.value().raw(), 1234567);
  PO_REQUIRE_ERR(parse_power("0.0001 W"), ReasonCode::ValueOutOfRange);
  PO_REQUIRE_ERR(parse_power("not a number"), ReasonCode::ParseError);
}

PO_TEST(quantity, compact_duration_rendering) {
  PO_CHECK_EQ(to_compact_string(milliseconds(1500)), std::string("1.5s"));
  PO_CHECK_EQ(to_compact_string(milliseconds(250)), std::string("250ms"));
  PO_CHECK_EQ(to_compact_string(nanoseconds(7)), std::string("7ns"));
}

PO_TEST(time, iso8601_round_trip) {
  const Result<Timestamp> parsed = Timestamp::from_iso8601("2026-01-15T08:30:00.000000000Z");
  PO_REQUIRE_OK(parsed);
  PO_CHECK_EQ(parsed.value().to_iso8601(), std::string("2026-01-15T08:30:00.000000000Z"));
}

PO_TEST(time, iso8601_offset_normalises_to_utc) {
  const Result<Timestamp> parsed = Timestamp::from_iso8601("2026-01-15T10:30:00+02:00");
  PO_REQUIRE_OK(parsed);
  PO_CHECK_EQ(parsed.value().to_iso8601(), std::string("2026-01-15T08:30:00.000000000Z"));
}

PO_TEST(time, iso8601_rejects_invalid_fields) {
  PO_REQUIRE_ERR(Timestamp::from_iso8601("2026-02-30T00:00:00Z"), ReasonCode::ValueOutOfRange);
  PO_REQUIRE_ERR(Timestamp::from_iso8601("2026-01-15T24:00:00Z"), ReasonCode::ValueOutOfRange);
  PO_REQUIRE_ERR(Timestamp::from_iso8601("2026-01-15T00:00:60Z"), ReasonCode::ValueOutOfRange);
  PO_REQUIRE_ERR(Timestamp::from_iso8601("2026-01-15"), ReasonCode::ParseError);
}

PO_TEST(reason, table_is_sorted_and_complete) {
  PO_CHECK(reason_code_count() > 100);
  for (std::size_t index = 1; index < reason_code_count(); ++index) {
    PO_CHECK(static_cast<std::uint16_t>(reason_code_at(index - 1)) <
             static_cast<std::uint16_t>(reason_code_at(index)));
  }
  PO_CHECK_EQ(to_string(ReasonCode::SplitBrainEvidence), std::string_view("split_brain_evidence"));
  PO_CHECK_EQ(to_string(ReasonCode::IoError), std::string_view("io_error"));
}

PO_TEST(result, carries_value_or_error) {
  const Result<int> good = 7;
  PO_CHECK(good.has_value());
  PO_CHECK_EQ(good.value(), 7);
  const Result<int> bad = Error(ReasonCode::NotFound, "missing");
  PO_CHECK(!bad.has_value());
  PO_CHECK_EQ(bad.code(), ReasonCode::NotFound);
}

PO_TEST(version, build_info_is_ascii_and_mentions_project) {
  const std::string_view info = build_info();
  PO_CHECK(info.find("Power Observatory") != std::string_view::npos);
  for (const char character : info) {
    PO_CHECK(static_cast<unsigned char>(character) < 128);
  }
}

PO_TEST_MAIN()
