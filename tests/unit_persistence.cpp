// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/crc32c.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/persistence.hpp"
#include "power_observatory/version.hpp"

using namespace po;

namespace {

EvidenceBatch make_batch(std::uint64_t mutation, std::uint64_t sequence, QuantityRep watts) {
  EvidenceBatch batch;
  batch.source = SourceId("meter-1");
  batch.authority = AuthorityKind::Observed;
  batch.generation = Generation(1);
  batch.epoch = Epoch{};
  batch.first_sequence = Sequence(sequence);
  batch.mutation = MutationId(mutation);
  batch.attempt = AttemptId(1);
  batch.recorded_at = Timestamp::from_unix_millis(1700000000000LL).value();

  Measurement measurement;
  measurement.id = MeasurementId(sequence);
  measurement.entity = EntityRef::feed(FeedId("a"));
  measurement.phase = Phase::Total;
  measurement.value = Power::from_raw(watts);
  measurement.provenance = make_observed_provenance(SourceId("meter-1"), Generation(1), Epoch{}, Sequence(sequence),
                                                    std::nullopt, AuthorityKind::Observed,
                                                    batch.recorded_at, MonotonicInstant::from_nanos(1));
  batch.measurements.push_back(std::move(measurement));
  return batch;
}

std::string new_log(std::string_view suite) {
  const std::string directory = pofix::scratch(suite);
  return join_path(directory, "evidence.poev");
}

// Rewrites one span of a file in place. Used to simulate the byte-level damage
// a failing device or a hostile writer would produce.
void overwrite_bytes(const std::string& path, std::uint64_t offset, const std::vector<std::uint8_t>& bytes) {
  const Result<std::vector<std::uint8_t>> current = read_whole_file(path);
  PO_REQUIRE(current.has_value());
  std::vector<std::uint8_t> whole = current.value();
  PO_REQUIRE(offset + bytes.size() <= whole.size());
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    whole[static_cast<std::size_t>(offset) + index] = bytes[index];
  }
  PO_CHECK_OK(remove_file(path));
  Result<FileHandle> fresh = FileHandle::create_new(path, true);
  PO_REQUIRE(fresh.has_value());
  PO_CHECK_OK(fresh.value().append(whole.data(), whole.size()));
  PO_CHECK_OK(fresh.value().sync());
  PO_CHECK_OK(fresh.value().close());
}

}  // namespace

PO_TEST(persistence, fresh_log_has_a_valid_header) {
  const std::string path = new_log("persistence_header");
  StoreOpenOptions options;
  Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{3}, options);
  PO_REQUIRE_OK(log);
  PO_CHECK(log.value().recovery().created_new);
  PO_CHECK(log.value().recovery().header_valid);
  PO_CHECK_EQ(log.value().epoch().value(), std::uint64_t{3});
  PO_CHECK_EQ(log.value().committed_length(), std::uint64_t{kEvidenceLogHeaderSize});
  PO_CHECK_OK(log.value().close());
  PO_REQUIRE(file_exists(path));
  pofix::clear("persistence_header");
}

PO_TEST(persistence, commit_then_reopen_recovers_the_same_evidence) {
  const std::string path = new_log("persistence_roundtrip");
  StoreOpenOptions options;
  std::uint64_t hash = 0;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_REQUIRE_OK(log.value().append(make_batch(1, 1, 111)));
    PO_REQUIRE_OK(log.value().append(make_batch(2, 2, 222)));
    hash = log.value().evidence().content_hash();
    PO_CHECK_OK(log.value().close());
  }
  {
    Result<EvidenceLog> reopened = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(reopened);
    PO_CHECK(!reopened.value().recovery().created_new);
    PO_CHECK_EQ(reopened.value().recovery().records_applied, std::size_t{2});
    PO_CHECK_EQ(reopened.value().recovery().bytes_discarded, std::uint64_t{0});
    PO_CHECK_EQ(reopened.value().evidence().size(), std::size_t{1});
    // The content hash deliberately covers the provenance origin, so the live
    // and recovered views of the same evidence are expected to differ on that
    // one field and to agree on everything else.
    PO_CHECK_NE(reopened.value().evidence().content_hash(), hash);
    const Measurement& recovered = reopened.value().evidence().measurements().front();
    PO_CHECK_EQ(recovered.entity.to_string(), std::string("feed:a"));
    PO_CHECK_EQ(std::get<Power>(recovered.value).raw(), QuantityRep{222});
    PO_CHECK_EQ(recovered.provenance.origin, EvidenceOrigin::RecoveredFromStore);
    PO_CHECK_OK(reopened.value().close());
  }
  pofix::clear("persistence_roundtrip");
}

PO_TEST(persistence, recovered_measurements_are_marked_recovered) {
  const std::string path = new_log("persistence_recovered");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_REQUIRE_OK(log.value().append(make_batch(1, 1, 111)));
    PO_CHECK_OK(log.value().close());
  }
  Result<EvidenceLog> reopened = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_OK(reopened);
  PO_REQUIRE(reopened.value().evidence().size() == 1);
  const Measurement& measurement = reopened.value().evidence().measurements().front();
  PO_CHECK(measurement.provenance.recovered());
  PO_CHECK(!measurement.provenance.has_monotonic_anchor);
  PO_CHECK_EQ(measurement.provenance.origin, EvidenceOrigin::RecoveredFromStore);
  PO_CHECK_EQ(measurement.provenance.authority, AuthorityKind::Observed);
  PO_CHECK_OK(reopened.value().close());
  pofix::clear("persistence_recovered");
}

PO_TEST(persistence, idempotent_replay_is_refused_and_not_applied_twice) {
  const std::string path = new_log("persistence_replay");
  StoreOpenOptions options;
  Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_OK(log);
  PO_REQUIRE_OK(log.value().append(make_batch(7, 1, 111)));
  const Status second = log.value().append(make_batch(7, 2, 222));
  PO_REQUIRE_ERR(second, ReasonCode::IdempotentReplay);
  PO_CHECK_EQ(log.value().evidence().size(), std::size_t{1});
  PO_CHECK_OK(log.value().close());
  pofix::clear("persistence_replay");
}

PO_TEST(persistence, reopening_at_an_older_epoch_is_refused) {
  const std::string path = new_log("persistence_epoch");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch(5), options);
    PO_REQUIRE_OK(log);
    PO_CHECK_OK(log.value().close());
  }
  Result<EvidenceLog> older = EvidenceLog::open(path, Epoch(4), options);
  PO_REQUIRE_ERR(older, ReasonCode::EpochMismatch);
  Result<EvidenceLog> same = EvidenceLog::open(path, Epoch(5), options);
  PO_REQUIRE_OK(same);
  PO_CHECK_OK(same.value().close());
  pofix::clear("persistence_epoch");
}

PO_TEST(persistence, epoch_advance_must_move_forward) {
  const std::string path = new_log("persistence_advance");
  StoreOpenOptions options;
  Result<EvidenceLog> log = EvidenceLog::open(path, Epoch(2), options);
  PO_REQUIRE_OK(log);
  PO_REQUIRE_ERR(log.value().advance_epoch(Epoch(2), "same"), ReasonCode::EpochRegression);
  PO_REQUIRE_ERR(log.value().advance_epoch(Epoch(1), "backwards"), ReasonCode::EpochRegression);
  PO_REQUIRE_OK(log.value().advance_epoch(Epoch(3), "operator takeover"));
  PO_CHECK_EQ(log.value().epoch().value(), std::uint64_t{3});
  PO_CHECK_OK(log.value().close());
  pofix::clear("persistence_advance");
}

PO_TEST(persistence, a_batch_at_a_stale_epoch_is_refused) {
  const std::string path = new_log("persistence_stale_epoch");
  StoreOpenOptions options;
  Result<EvidenceLog> log = EvidenceLog::open(path, Epoch(4), options);
  PO_REQUIRE_OK(log);
  EvidenceBatch stale = make_batch(1, 1, 100);
  stale.epoch = Epoch(3);
  PO_REQUIRE_ERR(log.value().append(stale), ReasonCode::StaleEpoch);
  EvidenceBatch ahead = make_batch(2, 2, 100);
  ahead.epoch = Epoch(5);
  PO_REQUIRE_ERR(log.value().append(ahead), ReasonCode::EpochMismatch);
  PO_CHECK_OK(log.value().close());
  pofix::clear("persistence_stale_epoch");
}

PO_TEST(persistence, torn_tail_is_discarded_and_only_the_tail) {
  const std::string path = new_log("persistence_torn_tail");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_REQUIRE_OK(log.value().append(make_batch(1, 1, 111)));
    PO_REQUIRE_OK(log.value().append(make_batch(2, 2, 222)));
    PO_CHECK_OK(log.value().close());
  }
  const Result<std::uint64_t> before = file_size(path);
  PO_REQUIRE_OK(before);

  // Simulate a process that died midway through writing a third record.
  {
    Result<FileHandle> handle = FileHandle::open_append(path);
    PO_REQUIRE_OK(handle);
    const std::vector<std::uint8_t> partial{0x50, 0x4F, 0x52, 0x31, 0x01, 0x00, 0x00, 0x00, 0x40};
    PO_CHECK_OK(handle.value().append(partial.data(), partial.size()));
    PO_CHECK_OK(handle.value().sync());
    PO_CHECK_OK(handle.value().close());
  }

  Result<EvidenceLog> reopened = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_OK(reopened);
  PO_CHECK_EQ(reopened.value().recovery().records_applied, std::size_t{2});
  PO_CHECK_EQ(reopened.value().recovery().bytes_discarded, std::uint64_t{9});
  PO_CHECK(reopened.value().recovery().truncated);
  PO_CHECK_EQ(reopened.value().recovery().tail_reason, ReasonCode::TruncatedRecord);
  PO_CHECK_EQ(reopened.value().evidence().size(), std::size_t{1});
  PO_CHECK_OK(reopened.value().close());

  const Result<std::uint64_t> after = file_size(path);
  PO_REQUIRE_OK(after);
  PO_CHECK_EQ(after.value(), before.value());
  pofix::clear("persistence_torn_tail");
}

PO_TEST(persistence, interior_corruption_is_refused_rather_than_truncated) {
  const std::string path = new_log("persistence_interior");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_REQUIRE_OK(log.value().append(make_batch(1, 1, 111)));
    PO_REQUIRE_OK(log.value().append(make_batch(2, 2, 222)));
    PO_REQUIRE_OK(log.value().append(make_batch(3, 3, 333)));
    PO_CHECK_OK(log.value().close());
  }
  // Flip one payload byte of the first record. A valid record still follows, so
  // this is corruption in the interior and must not be trimmed away.
  overwrite_bytes(path, kEvidenceLogHeaderSize + kEvidenceLogRecordHeaderSize + 20, {0xFF});

  Result<EvidenceLog> reopened = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE(!reopened.has_value());
  PO_CHECK_EQ(reopened.code(), ReasonCode::InteriorCorruption);
  PO_CHECK(!reopened.detail().empty());
  pofix::clear("persistence_interior");
}

PO_TEST(persistence, a_damaged_file_header_is_unrecoverable) {
  const std::string path = new_log("persistence_header_damage");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_CHECK_OK(log.value().close());
  }
  overwrite_bytes(path, 0, {0x00});
  Result<EvidenceLog> reopened = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_ERR(reopened, ReasonCode::IntegrityMismatch);
  pofix::clear("persistence_header_damage");
}

PO_TEST(persistence, an_unsupported_format_version_is_refused) {
  const std::string path = new_log("persistence_version");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_CHECK_OK(log.value().close());
  }
  overwrite_bytes(path, 8, {0x7F, 0x00});
  Result<EvidenceLog> reopened = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_ERR(reopened, ReasonCode::FormatVersionUnsupported);
  pofix::clear("persistence_version");
}

PO_TEST(persistence, a_foreign_file_is_not_mistaken_for_a_log) {
  const std::string path = new_log("persistence_foreign");
  PO_REQUIRE_OK(write_file_atomically(path, std::string_view("this is not an evidence log, it is prose")));
  StoreOpenOptions options;
  Result<EvidenceLog> opened = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_ERR(opened, ReasonCode::IntegrityMismatch);
  pofix::clear("persistence_foreign");
}

PO_TEST(persistence, inspect_does_not_modify_and_reports_the_same_records) {
  const std::string path = new_log("persistence_inspect");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_REQUIRE_OK(log.value().append(make_batch(1, 1, 111)));
    PO_CHECK_OK(log.value().close());
  }
  const Result<std::uint64_t> before = file_size(path);
  PO_REQUIRE_OK(before);
  const Result<RecoveryReport> report = EvidenceLog::inspect(path, options);
  PO_REQUIRE_OK(report);
  PO_CHECK_EQ(report.value().records_scanned, std::size_t{1});
  PO_CHECK_EQ(report.value().records_applied, std::size_t{1});
  const Result<std::uint64_t> after = file_size(path);
  PO_REQUIRE_OK(after);
  PO_CHECK_EQ(before.value(), after.value());
  pofix::clear("persistence_inspect");
}

PO_TEST(persistence, read_only_load_reports_a_torn_tail_without_repairing_it) {
  const std::string path = new_log("persistence_readonly");
  StoreOpenOptions options;
  {
    Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
    PO_REQUIRE_OK(log);
    PO_REQUIRE_OK(log.value().append(make_batch(1, 1, 111)));
    PO_CHECK_OK(log.value().close());
  }
  {
    Result<FileHandle> handle = FileHandle::open_append(path);
    PO_REQUIRE_OK(handle);
    const std::vector<std::uint8_t> partial{0x50, 0x4F, 0x52};
    PO_CHECK_OK(handle.value().append(partial.data(), partial.size()));
    PO_CHECK_OK(handle.value().close());
  }
  const Result<std::uint64_t> before = file_size(path);
  PO_REQUIRE_OK(before);

  const Result<EvidenceLog::Loaded> loaded = EvidenceLog::load(path, options);
  PO_REQUIRE_OK(loaded);
  PO_CHECK_EQ(loaded.value().evidence.size(), std::size_t{1});
  PO_CHECK_EQ(loaded.value().report.bytes_discarded, std::uint64_t{3});
  PO_CHECK(!loaded.value().report.truncated);
  const Result<std::uint64_t> after = file_size(path);
  PO_REQUIRE_OK(after);
  PO_CHECK_EQ(before.value(), after.value());
  pofix::clear("persistence_readonly");
}

PO_TEST(persistence, oversized_payload_is_refused) {
  const std::string path = new_log("persistence_oversized");
  StoreOpenOptions options;
  options.max_measurements_per_batch = 1;
  Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_OK(log);
  EvidenceBatch batch = make_batch(1, 1, 111);
  Measurement extra = batch.measurements.front();
  extra.id = MeasurementId(2);
  batch.measurements.push_back(extra);
  PO_REQUIRE_ERR(log.value().append(batch), ReasonCode::ValueOutOfRange);
  PO_CHECK_OK(log.value().close());
  pofix::clear("persistence_oversized");
}

PO_TEST(persistence, a_batch_without_a_source_is_refused) {
  const std::string path = new_log("persistence_no_source");
  StoreOpenOptions options;
  Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
  PO_REQUIRE_OK(log);
  EvidenceBatch batch = make_batch(1, 1, 111);
  batch.source = SourceId("");
  PO_REQUIRE_ERR(log.value().append(batch), ReasonCode::SchemaViolation);
  PO_CHECK_OK(log.value().close());
  pofix::clear("persistence_no_source");
}

PO_TEST_MAIN()
