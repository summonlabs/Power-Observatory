// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// End-to-end coverage of the public command line surface, driven through the
// same entry point the executable uses. Every command is exercised in the order
// an operator would run it, and the published documents are parsed back so that
// the contract is checked rather than the formatting.
#include "test_harness.hpp"

#include <sstream>
#include <string>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/cli.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/json.hpp"

using namespace po;

namespace {

struct Run {
  int exit_code{0};
  std::string out;
  std::string err;
};

Run run(const std::vector<std::string>& arguments) {
  std::ostringstream out;
  std::ostringstream err;
  Run result;
  result.exit_code = run_cli(arguments, out, err);
  result.out = out.str();
  result.err = err.str();
  return result;
}

[[nodiscard]] Result<JsonValue> parse_output(const Run& run_result) { return JsonValue::parse(run_result.out); }

}  // namespace

PO_TEST(e2e, the_whole_operator_workflow_holds_together) {
  const std::string store = pofix::join("e2e_workflow", "site");
  const std::string samples = pofix::join("e2e_workflow", "samples.jsonl");

  const Run version = run({"version"});
  PO_CHECK_EQ(version.exit_code, 0);
  const Result<JsonValue> version_document = parse_output(version);
  PO_REQUIRE_OK(version_document);
  PO_CHECK_EQ(version_document.value().find("version")->as_string(), std::string("1.0.0"));

  const Run selfcheck = run({"selfcheck"});
  PO_CHECK_EQ(selfcheck.exit_code, 0);
  const Result<JsonValue> selfcheck_document = parse_output(selfcheck);
  PO_REQUIRE_OK(selfcheck_document);
  PO_CHECK(selfcheck_document.value().find("passed")->as_boolean());

  PO_CHECK_EQ(run({"init", "--store", store, "--seed", "17"}).exit_code, 0);
  PO_CHECK_EQ(run({"scenario", "--store", store, "--out", samples, "--steps", "3"}).exit_code, 0);
  PO_CHECK_EQ(run({"ingest", "--store", store, "--input", samples}).exit_code, 0);

  // A repeated ingest is a real retry and must be recognised as such.
  const Run again = run({"ingest", "--store", store, "--input", samples});
  PO_CHECK_EQ(again.exit_code, 0);
  const Result<JsonValue> again_document = parse_output(again);
  PO_REQUIRE_OK(again_document);
  if (again_document.value().find("idempotent_replay")->as_u64() == 0) {
    PO_FAIL("second ingest document: " + again_document.value().dump());
  }

  // Verification must be clean after a double ingest.
  const Run verify = run({"verify", "--store", store});
  PO_CHECK_EQ(verify.exit_code, 0);
  const Result<JsonValue> verify_document = parse_output(verify);
  PO_REQUIRE_OK(verify_document);
  PO_CHECK(verify_document.value().find("clean")->as_boolean());
  PO_CHECK(verify_document.value().find("measurements")->as_u64() > 0);
  if (verify_document.value().find("measurements")->as_u64() == 0) {
    PO_FAIL("verify document: " + verify_document.value().dump());
  }

  const Run answer = run({"answer", "--store", store});
  PO_CHECK_EQ(answer.exit_code, 0);
  const Result<JsonValue> answer_document = parse_output(answer);
  PO_REQUIRE_OK(answer_document);
  // A store read by a fresh process holds recovered evidence, so the answer is
  // the last known state rather than the current one, and it says so.
  PO_CHECK_EQ(answer_document.value().find("state")->as_string(), std::string("recovered"));
  if (answer_document.value().find("state")->as_string() != "recovered") {
    PO_FAIL("answer document: " + answer_document.value().dump());
  }
  PO_CHECK(answer_document.value().find("total_observed_load_w")->type() == JsonValue::Type::String);
  PO_CHECK(!answer_document.value().find("explanation")->find("reasons")->as_array().empty());

  for (const std::string& command : {"flow", "attribution", "quality", "explain", "topology", "history"}) {
    const Run result = run({command, "--store", store});
    PO_CHECK_EQ(result.exit_code, 0);
    PO_CHECK(!result.out.empty());
  }

  const Run reserve = run({"reserve", "--store", store, "--scope", "redundancy_group:rg-main"});
  PO_CHECK_EQ(reserve.exit_code, 0);
  const Result<JsonValue> reserve_document = parse_output(reserve);
  PO_REQUIRE_OK(reserve_document);
  PO_CHECK_EQ(reserve_document.value().find("state")->as_string(), std::string("known"));

  const Run failover = run({"failover", "--store", store, "--group", "rg-main"});
  PO_CHECK_EQ(failover.exit_code, 0);
  const Result<JsonValue> failover_document = parse_output(failover);
  PO_REQUIRE_OK(failover_document);
  PO_CHECK(!failover_document.value().find("gates")->as_array().empty());

  // Two output formats must describe the same thing.
  const Run text = run({"answer", "--store", store, "--text"});
  PO_CHECK_EQ(text.exit_code, 0);
  PO_CHECK(text.out.find("state: recovered") != std::string::npos);
  PO_CHECK(text.out.find("evaluated as of:") != std::string::npos);

  pofix::clear("e2e_workflow");
}

PO_TEST(e2e, usage_errors_are_reported_as_usage_and_refusals_as_refusals) {
  PO_CHECK_EQ(run({}).exit_code, 1);
  PO_CHECK_EQ(run({"nonsense"}).exit_code, 1);
  PO_CHECK_EQ(run({"answer"}).exit_code, 1);
  PO_CHECK_EQ(run({"reserve", "--store", "missing-store", "--scope", "feed:a"}).exit_code, 2);
  PO_CHECK_EQ(run({"answer", "--store", "missing-store"}).exit_code, 2);
  PO_CHECK_EQ(run({"divergence", "--left-store", "a"}).exit_code, 1);
  PO_CHECK_EQ(run({"--help"}).exit_code, 0);

  const Run unknown = run({"nonsense"});
  PO_CHECK(unknown.err.find("unrecognized command") != std::string::npos);
  PO_CHECK(unknown.err.find("usage:") != std::string::npos);
}

PO_TEST(e2e, a_query_at_an_explicit_instant_is_honoured) {
  const std::string store = pofix::join("e2e_as_of", "site");
  const std::string samples = pofix::join("e2e_as_of", "samples.jsonl");
  PO_CHECK_EQ(run({"init", "--store", store, "--seed", "3"}).exit_code, 0);
  PO_CHECK_EQ(run({"scenario", "--store", store, "--out", samples, "--steps", "2"}).exit_code, 0);
  PO_CHECK_EQ(run({"ingest", "--store", store, "--input", samples}).exit_code, 0);

  // Evaluating a year after the evidence was recorded must report that the
  // evidence has expired rather than presenting it as current.
  const Run old = run({"answer", "--store", store, "--as-of", "2027-01-01T00:00:00Z"});
  PO_CHECK_EQ(old.exit_code, 0);
  const Result<JsonValue> document = parse_output(old);
  PO_REQUIRE_OK(document);
  PO_CHECK_EQ(document.value().find("state")->as_string(), std::string("expired"));

  const Run now = run({"answer", "--store", store, "--now"});
  PO_CHECK_EQ(now.exit_code, 0);

  const Run bad = run({"answer", "--store", store, "--as-of", "not-a-timestamp"});
  PO_CHECK_EQ(bad.exit_code, 1);
  pofix::clear("e2e_as_of");
}

PO_TEST_MAIN()