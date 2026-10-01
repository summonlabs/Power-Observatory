// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Adversarial storage: every byte of a committed log is damaged in turn and the
// recovery rules are required to hold. A single flipped bit must never be
// silently accepted, and damage that is not at the tail must never be trimmed
// away.
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/persistence.hpp"
#include "power_observatory/version.hpp"

using namespace po;

namespace {

EvidenceBatch batch_for(std::uint64_t mutation, std::uint64_t sequence, QuantityRep watts) {
  EvidenceBatch batch;
  batch.source = SourceId("meter-1");
  batch.authority = AuthorityKind::Observed;
  batch.generation = Generation(1);
  batch.first_sequence = Sequence(sequence);
  batch.mutation = MutationId(mutation);
  batch.attempt = AttemptId(1);
  batch.recorded_at = Timestamp::from_unix_millis(1700000000000LL).value();

  Measurement measurement;
  measurement.id = MeasurementId(sequence);
  measurement.entity = EntityRef::feed(FeedId("a"));
  measurement.phase = Phase::Total;
  measurement.value = Power::from_raw(watts);
  measurement.provenance =
      make_observed_provenance(SourceId("meter-1"), Generation(1), Epoch{}, Sequence(sequence), std::nullopt,
                               AuthorityKind::Observed, batch.recorded_at, MonotonicInstant::from_nanos(1));
  batch.measurements.push_back(std::move(measurement));
  return batch;
}

std::string build_log(std::string_view suite) {
  const std::string directory = pofix::scratch(suite);
  const std::string path = join_path(directory, "evidence.poev");
  StoreOpenOptions options;
  Result<EvidenceLog> log = EvidenceLog::open(path, Epoch{}, options);
  if (!log) {
    return std::string();
  }
  for (std::uint64_t index = 0; index < 3; ++index) {
    if (!log.value().append(batch_for(index + 1, index + 1, 1000 + index))) {
      return std::string();
    }
  }
  static_cast<void>(log.value().close());
  return path;
}

void patch(const std::string& path, std::uint64_t offset, std::uint8_t value) {
  const Result<std::vector<std::uint8_t>> current = read_whole_file(path);
  PO_REQUIRE(current.has_value());
  std::vector<std::uint8_t> whole = current.value();
  PO_REQUIRE(offset < whole.size());
  whole[static_cast<std::size_t>(offset)] = value;
  PO_CHECK_OK(remove_file(path));
  Result<FileHandle> fresh = FileHandle::create_new(path, true);
  PO_REQUIRE(fresh.has_value());
  PO_CHECK_OK(fresh.value().append(whole.data(), whole.size()));
  PO_CHECK_OK(fresh.value().sync());
  PO_CHECK_OK(fresh.value().close());
}

}  // namespace

PO_TEST(adversarial, a_single_bit_flip_in_a_payload_is_never_silently_accepted) {
  // Damage one byte of the middle record's payload. The log must refuse to
  // open, because a well-formed record still follows the damage.
  const std::string path = build_log("adversarial_bit_flip");
  PO_REQUIRE(!path.empty());
  const Result<std::uint64_t> size = file_size(path);
  PO_REQUIRE_OK(size);

  // Locate the second record's payload by walking the headers.
  const Result<std::vector<std::uint8_t>> bytes = read_whole_file(path);
  PO_REQUIRE_OK(bytes);
  std::uint64_t offset = kEvidenceLogHeaderSize;
  std::vector<std::uint64_t> payload_offsets;
  while (offset + kEvidenceLogRecordHeaderSize <= bytes.value().size()) {
    const std::uint8_t* header = bytes.value().data() + offset;
    std::uint32_t payload_length = 0;
    payload_length = static_cast<std::uint32_t>(header[8]) | (static_cast<std::uint32_t>(header[9]) << 8) |
                     (static_cast<std::uint32_t>(header[10]) << 16) |
                     (static_cast<std::uint32_t>(header[11]) << 24);
    if (payload_length == 0 || offset + kEvidenceLogRecordHeaderSize + payload_length > bytes.value().size()) {
      break;
    }
    payload_offsets.push_back(offset + kEvidenceLogRecordHeaderSize);
    offset += kEvidenceLogRecordHeaderSize + payload_length;
  }
  PO_REQUIRE(payload_offsets.size() == 3);
  PO_REQUIRE_OK(size);

  // Damage in the interior: a well-formed record follows, so the log is refused.
  for (std::size_t index = 0; index + 1 < payload_offsets.size(); ++index) {
    const std::string candidate = path + ".interior";
    PO_CHECK_OK(write_file_atomically(candidate, std::string_view(
        reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size())));
    const std::uint64_t payload_offset = payload_offsets[index];
    patch(candidate, payload_offset + 3, static_cast<std::uint8_t>(bytes.value()[payload_offset + 3] ^ 0x01));
    StoreOpenOptions options;
    const Result<EvidenceLog> opened = EvidenceLog::open(candidate, Epoch{}, options);
    PO_CHECK(!opened.has_value());
    PO_CHECK_EQ(opened.code(), ReasonCode::InteriorCorruption);
    PO_CHECK_OK(remove_file(candidate));
    PO_CHECK_OK(remove_file(candidate + ".lock"));
  }

  // Damage to the final payload has nothing after it, so it is a torn tail: it
  // is discarded, and the two records before it survive intact.
  {
    const std::string candidate = path + ".tail_damage";
    PO_CHECK_OK(write_file_atomically(candidate, std::string_view(
        reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size())));
    const std::uint64_t payload_offset = payload_offsets.back();
    patch(candidate, payload_offset + 3, static_cast<std::uint8_t>(bytes.value()[payload_offset + 3] ^ 0x01));
    StoreOpenOptions options;
    Result<EvidenceLog> opened = EvidenceLog::open(candidate, Epoch{}, options);
    PO_REQUIRE_OK(opened);
    PO_CHECK_EQ(opened.value().recovery().records_applied, std::size_t{2});
    PO_CHECK(opened.value().recovery().bytes_discarded > 0);
    PO_CHECK_EQ(opened.value().recovery().tail_reason, ReasonCode::CorruptRecord);
    PO_CHECK_OK(opened.value().close());
    PO_CHECK_OK(remove_file(candidate));
    PO_CHECK_OK(remove_file(candidate + ".lock"));
  }
  pofix::clear("adversarial_bit_flip");
}

PO_TEST(adversarial, damage_confined_to_the_tail_is_recovered_and_only_the_tail) {
  const std::string path = build_log("adversarial_tail");
  PO_REQUIRE(!path.empty());
  const Result<std::vector<std::uint8_t>> bytes = read_whole_file(path);
  PO_REQUIRE_OK(bytes);

  // The committed length is the offset just past the second record. The third
  // record is the one that gets trimmed, and an incomplete record is discarded
  // in full, from its header, so the discarded count is its whole length minus
  // however many bytes were removed from it.
  const auto record_length_at = [&bytes](std::uint64_t offset) -> std::uint64_t {
    const std::uint8_t* header = bytes.value().data() + offset;
    return static_cast<std::uint64_t>(kEvidenceLogRecordHeaderSize) +
           (static_cast<std::uint64_t>(header[8]) | (static_cast<std::uint64_t>(header[9]) << 8) |
            (static_cast<std::uint64_t>(header[10]) << 16) | (static_cast<std::uint64_t>(header[11]) << 24));
  };
  std::uint64_t committed_length = kEvidenceLogHeaderSize;
  for (int index = 0; index < 2; ++index) {
    committed_length += record_length_at(committed_length);
  }

  for (std::uint64_t trim = 1; trim <= 24; ++trim) {
    const std::string candidate = path + ".trimmed";
    const std::size_t kept = bytes.value().size() - static_cast<std::size_t>(trim);
    PO_CHECK_OK(remove_file(candidate));
    Result<FileHandle> handle = FileHandle::create_new(candidate, true);
    PO_REQUIRE_OK(handle);
    PO_CHECK_OK(handle.value().append(bytes.value().data(), kept));
    PO_CHECK_OK(handle.value().sync());
    PO_CHECK_OK(handle.value().close());

    StoreOpenOptions options;
    Result<EvidenceLog> opened = EvidenceLog::open(candidate, Epoch{}, options);
    PO_REQUIRE_OK(opened);
    PO_CHECK_EQ(opened.value().recovery().records_applied, std::size_t{2});
    // The incomplete last record is discarded in full, from its header, because
    // a record is only ever admitted whole. What is discarded is therefore the
    // record's own length, which is the file size minus the committed length.
    PO_CHECK_EQ(opened.value().recovery().bytes_discarded,
                bytes.value().size() - committed_length - trim);
    PO_CHECK(opened.value().recovery().truncated);
    PO_CHECK_OK(opened.value().close());
    PO_CHECK_OK(remove_file(candidate));
    PO_CHECK_OK(remove_file(candidate + ".lock"));
  }
  pofix::clear("adversarial_tail");
}

PO_TEST(adversarial, truncation_inside_the_first_record_leaves_an_empty_but_valid_log) {
  const std::string path = build_log("adversarial_first_record");
  PO_REQUIRE(!path.empty());
  const Result<std::vector<std::uint8_t>> bytes = read_whole_file(path);
  PO_REQUIRE_OK(bytes);
  const std::string candidate = path + ".short";
  PO_CHECK_OK(remove_file(candidate));
  Result<FileHandle> handle = FileHandle::create_new(candidate, true);
  PO_REQUIRE_OK(handle);
  const std::size_t kept = kEvidenceLogHeaderSize + 4;
  PO_CHECK_OK(handle.value().append(bytes.value().data(), kept));
  PO_CHECK_OK(handle.value().sync());
  PO_CHECK_OK(handle.value().close());

  StoreOpenOptions options;
  Result<EvidenceLog> opened = EvidenceLog::open(candidate, Epoch{}, options);
  PO_REQUIRE_OK(opened);
  PO_CHECK_EQ(opened.value().recovery().records_applied, std::size_t{0});
  PO_CHECK_EQ(opened.value().evidence().size(), std::size_t{0});
  PO_CHECK_EQ(opened.value().committed_length(), std::uint64_t{kEvidenceLogHeaderSize});
  PO_CHECK_OK(opened.value().close());
  pofix::clear("adversarial_first_record");
}

PO_TEST(adversarial, a_zeroed_file_is_refused) {
  const std::string path = build_log("adversarial_zeroed");
  PO_REQUIRE(!path.empty());
  const Result<std::uint64_t> size = file_size(path);
  PO_REQUIRE_OK(size);
  const Result<std::vector<std::uint8_t>> bytes = read_whole_file(path);
  PO_REQUIRE_OK(bytes);
  std::vector<std::uint8_t> zeroed(bytes.value().size(), 0);
  PO_CHECK_OK(write_file_atomically(path, zeroed.data(), zeroed.size()));
  StoreOpenOptions options;
  PO_REQUIRE_ERR(EvidenceLog::open(path, Epoch{}, options), ReasonCode::IntegrityMismatch);
  pofix::clear("adversarial_zeroed");
}

PO_TEST(adversarial, an_empty_file_is_refused) {
  const std::string directory = pofix::scratch("adversarial_empty");
  const std::string path = join_path(directory, "evidence.poev");
  PO_CHECK_OK(write_file_atomically(path, std::string_view("")));
  StoreOpenOptions options;
  PO_REQUIRE_ERR(EvidenceLog::open(path, Epoch{}, options), ReasonCode::IntegrityMismatch);
  pofix::clear("adversarial_empty");
}

PO_TEST_MAIN()
