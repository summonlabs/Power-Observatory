// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Real process death and real restart. Child processes commit evidence; the
// parent then reopens the same store and must recover exactly what was
// committed and nothing more. The children are launched directly, with no shell
// in between, so the paths under test are the paths actually used.
#include "test_harness.hpp"

#include <string>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/persistence.hpp"

using namespace po;

namespace {

#ifndef PO_CLI_PATH
#define PO_CLI_PATH "power-observatory"
#endif

[[nodiscard]] int run_cli_child(const std::vector<std::string>& arguments) {
  return pofix::run_child(std::string(PO_CLI_PATH), arguments);
}

}  // namespace

PO_TEST(failure, a_process_that_dies_mid_run_leaves_a_recoverable_store) {
  const std::string directory = pofix::scratch("failure_restart");
  const std::string store = join_path(directory, "site");
  const std::string samples = join_path(directory, "samples.jsonl");

  PO_CHECK_EQ(run_cli_child({"init", "--store", store, "--seed", "91", "--text"}), 0);
  PO_CHECK_EQ(run_cli_child({"scenario", "--store", store, "--out", samples, "--steps", "4", "--text"}), 0);
  PO_CHECK_EQ(run_cli_child({"ingest", "--store", store, "--input", samples, "--text"}), 0);

  // The writer lock lives in a lock file that the operating system releases when
  // the owning process ends, so an exited process never leaves it held.
  const std::string log_path = join_path(store, "evidence.poev");
  StoreOpenOptions options;
  const Result<EvidenceLog::Loaded> loaded = EvidenceLog::load(log_path, options);
  PO_REQUIRE_OK(loaded);
  PO_CHECK_EQ(loaded.value().report.records_applied, std::size_t{4});
  PO_CHECK_EQ(loaded.value().report.bytes_discarded, std::uint64_t{0});
  PO_CHECK(!loaded.value().evidence.empty());
  for (const Measurement& measurement : loaded.value().evidence.measurements()) {
    PO_CHECK(measurement.provenance.recovered());
  }
  const std::size_t before = loaded.value().evidence.size();
  const Result<std::uint64_t> length_before = file_size(log_path);
  PO_REQUIRE_OK(length_before);

  // Re-running the same ingest is a real retry against a real durable log. The
  // retry is refused at write time, so the durable stream must not grow.
  PO_CHECK_EQ(run_cli_child({"ingest", "--store", store, "--input", samples, "--text"}), 0);

  const Result<std::uint64_t> length_after = file_size(log_path);
  PO_REQUIRE_OK(length_after);
  PO_CHECK_EQ(length_after.value(), length_before.value());

  // A read-only load replays the durable stream from its first record, so every
  // record is applied again; what proves idempotency is that there are still
  // exactly four of them and that the evidence is unchanged.
  const Result<EvidenceLog::Loaded> after = EvidenceLog::load(log_path, options);
  PO_REQUIRE_OK(after);
  PO_CHECK_EQ(after.value().report.records_scanned, std::size_t{4});
  PO_CHECK_EQ(after.value().report.records_applied, std::size_t{4});
  PO_CHECK_EQ(after.value().report.records_skipped_replay, std::size_t{0});
  PO_CHECK_EQ(after.value().evidence.size(), before);

  // A third process reopens the store and must reach the same evidence.
  PO_CHECK_EQ(run_cli_child({"verify", "--store", store, "--text"}), 0);
  pofix::clear("failure_restart");
}

PO_TEST(failure, a_truncated_write_from_a_dead_process_is_repaired_on_reopen) {
  const std::string directory = pofix::scratch("failure_partial_write");
  const std::string store = join_path(directory, "site");
  const std::string samples = join_path(directory, "samples.jsonl");

  PO_CHECK_EQ(run_cli_child({"init", "--store", store, "--seed", "92", "--text"}), 0);
  PO_CHECK_EQ(run_cli_child({"scenario", "--store", store, "--out", samples, "--steps", "2", "--text"}), 0);
  PO_CHECK_EQ(run_cli_child({"ingest", "--store", store, "--input", samples, "--text"}), 0);

  const std::string log_path = join_path(store, "evidence.poev");
  StoreOpenOptions options;
  const Result<EvidenceLog::Loaded> baseline = EvidenceLog::load(log_path, options);
  PO_REQUIRE_OK(baseline);
  const std::size_t committed_records = baseline.value().report.records_applied;
  PO_REQUIRE(committed_records == 2);
  const std::size_t committed_measurements = baseline.value().evidence.size();

  // Append a partial record header, exactly as an interrupted write would.
  {
    Result<FileHandle> handle = FileHandle::open_append(log_path);
    PO_REQUIRE_OK(handle);
    const std::vector<std::uint8_t> partial{0x50, 0x4F, 0x52, 0x31, 0x01, 0x00, 0x00, 0x00, 0x40, 0x00};
    PO_CHECK_OK(handle.value().append(partial.data(), partial.size()));
    PO_CHECK_OK(handle.value().sync());
    PO_CHECK_OK(handle.value().close());
  }

  // A real reopen through the writer path repairs the tail.
  Result<EvidenceLog> log = EvidenceLog::open(log_path, Epoch{}, options);
  PO_REQUIRE_OK(log);
  PO_CHECK_EQ(log.value().recovery().records_applied, committed_records);
  PO_CHECK_EQ(log.value().recovery().bytes_discarded, std::uint64_t{10});
  PO_CHECK(log.value().recovery().truncated);
  PO_CHECK_EQ(log.value().evidence().size(), committed_measurements);
  PO_CHECK_OK(log.value().close());

  // And a fourth process agrees.
  PO_CHECK_EQ(run_cli_child({"verify", "--store", store, "--text"}), 0);
  pofix::clear("failure_partial_write");
}

PO_TEST_MAIN()