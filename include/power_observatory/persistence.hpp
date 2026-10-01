// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "power_observatory/evidence.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/file_lock.hpp"
#include "power_observatory/result.hpp"
#include "power_observatory/strong.hpp"
#include "power_observatory/version.hpp"

namespace po {

// On-disk identity of an evidence log. The magic encodes both the product and
// the record framing so that a foreign file is rejected rather than parsed.
inline constexpr std::uint32_t kEvidenceLogRecordMagic = 0x31524F50u;  // "POR1" little-endian
inline constexpr std::uint16_t kEvidenceLogHeaderSize = 64;
inline constexpr std::uint16_t kEvidenceLogRecordHeaderSize = 32;

enum class EvidenceRecordKind : std::uint8_t {
  Unknown = 0,
  MeasurementBatch = 1,
  EpochAdvance = 2,
};

[[nodiscard]] std::string_view to_string(EvidenceRecordKind kind) noexcept;

// One durable unit of evidence. The mutation identity travels inside the same
// record as the mutation, so a retry can be recognised from the durable stream
// alone without consulting any other state.
struct EvidenceBatch {
  SourceId source;
  AuthorityKind authority{AuthorityKind::Unknown};
  Generation generation{};
  Epoch epoch{};
  Sequence first_sequence{};
  MutationId mutation{};
  AttemptId attempt{};
  Timestamp recorded_at{};
  std::vector<Measurement> measurements;
};

struct StoreOpenOptions {
  bool create_if_missing{true};
  // When false a torn tail is reported but left in place, and the log opens
  // read-only in effect.
  bool truncate_torn_tail{true};
  std::size_t max_records{1000000};
  std::size_t max_payload_bytes{16u * 1024u * 1024u};
  std::size_t max_measurements_per_batch{100000};
  std::size_t max_identifier_bytes{1024};
  // Recovery reads the whole log into memory. Beyond this bound the log is
  // refused rather than allowed to exhaust the process.
  std::size_t max_recovery_bytes{256u * 1024u * 1024u};
};

// What recovery actually did. Every field is evidence, not decoration: the
// caller can reconstruct exactly which bytes were discarded and why.
struct RecoveryReport {
  bool created_new{false};
  bool header_valid{false};
  std::size_t records_scanned{0};
  std::size_t records_applied{0};
  std::size_t records_skipped_replay{0};
  std::size_t records_rejected{0};
  std::uint64_t bytes_discarded{0};
  std::uint64_t committed_length{0};
  bool truncated{false};
  ReasonCode tail_reason{ReasonCode::None};
  std::string tail_detail;
  Epoch epoch{};
  Generation highest_generation{};
  std::vector<EvidenceNote> notes;
};

// A versioned, CRC-32C protected, append-only evidence log guarded by a
// kernel-enforced single-writer lock.
//
// Commit point: a batch is durable once append() has written the complete
// record and flushed the file to stable storage. The in-memory evidence index
// is updated only after that flush succeeds, so a published in-memory state
// never claims more than the durable state contains.
//
// Recovery rules, in order:
//   * A damaged file header is unrecoverable and the log refuses to open.
//   * A record whose header or payload fails its CRC is a torn tail only when
//     no valid later record exists. Otherwise it is interior corruption and the
//     whole log is refused.
//   * A torn tail is discarded by truncation at the last good record boundary,
//     and only the trailing bytes are ever discarded.
//   * Records whose epoch is older than the log epoch are refused as replays.
//   * A batch carrying a mutation identity that is already durable is reported
//     as an idempotent replay and is not applied twice.
class EvidenceLog {
 public:
  EvidenceLog() = default;
  ~EvidenceLog();
  EvidenceLog(EvidenceLog&& other) noexcept;
  EvidenceLog& operator=(EvidenceLog&& other) noexcept;
  EvidenceLog(const EvidenceLog&) = delete;
  EvidenceLog& operator=(const EvidenceLog&) = delete;

  [[nodiscard]] static Result<EvidenceLog> open(const std::string& path, Epoch epoch,
                                                const StoreOpenOptions& options);

  [[nodiscard]] Status append(const EvidenceBatch& batch);
  [[nodiscard]] Status advance_epoch(Epoch epoch, std::string reason);
  [[nodiscard]] Status sync();
  [[nodiscard]] Status close();
  [[nodiscard]] bool is_open() const noexcept { return handle_.is_open(); }

  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  // The evidence admitted so far. The set is rebuilt lazily after a mutation,
  // so this accessor is cheap to call repeatedly and O(admitted) after an
  // append. The log is single-writer by contract; concurrent readers must
  // synchronise with the writer themselves.
  [[nodiscard]] const EvidenceSet& evidence() const;
  [[nodiscard]] Epoch epoch() const noexcept { return epoch_; }
  [[nodiscard]] RecordIndex next_record_index() const noexcept { return next_record_index_; }
  [[nodiscard]] std::uint64_t committed_length() const noexcept { return committed_length_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::size_t batch_count() const noexcept { return batch_count_; }

  // Reads and validates the durable stream without taking any lock and without
  // modifying anything. Used by the verifier and by tests.
  [[nodiscard]] static Result<RecoveryReport> inspect(const std::string& path, const StoreOpenOptions& options);

  // A read-only load: takes a shared lock, never truncates, and never creates.
  // A torn tail is reported but left in place, and the evidence that precedes
  // it is still returned because a reader must not mutate what it reads.
  struct Loaded {
    EvidenceSet evidence;
    RecoveryReport report;
    Epoch epoch{};
    RecordIndex next_record_index{};
  };
  [[nodiscard]] static Result<Loaded> load(const std::string& path, const StoreOpenOptions& options);

 private:
  [[nodiscard]] Status create_fresh(const std::string& path, Epoch epoch, const StoreOpenOptions& options);
  [[nodiscard]] Status recover(const StoreOpenOptions& options);

  struct DecodedRecord {
    EvidenceRecordKind kind{EvidenceRecordKind::Unknown};
    std::uint64_t record_index{0};
    EvidenceBatch batch;
    Epoch epoch{};
    std::string epoch_reason;
  };

  [[nodiscard]] static Result<std::vector<std::uint8_t>> encode_batch(const EvidenceBatch& batch,
                                                                      const StoreOpenOptions& options);
  [[nodiscard]] static Result<std::vector<std::uint8_t>> encode_epoch_advance(Epoch epoch,
                                                                              std::string_view reason,
                                                                              const StoreOpenOptions& options);
  [[nodiscard]] static Result<DecodedRecord> decode_record(const std::vector<std::uint8_t>& payload,
                                                           const StoreOpenOptions& options);
  [[nodiscard]] Status append_record(const std::vector<std::uint8_t>& record);

  std::string path_;
  std::string lock_path_;
  FileHandle handle_;
  FileLock lock_;
  RecoveryReport recovery_;
  std::vector<Measurement> admitted_;
  mutable EvidenceSet evidence_cache_;
  mutable bool evidence_dirty_{true};
  StoreOpenOptions options_{};
  Epoch epoch_{};
  RecordIndex next_record_index_{};
  std::uint64_t committed_length_{0};
  std::size_t batch_count_{0};
  std::vector<MutationId> committed_mutations_;
};

// Publishes a snapshot payload atomically: a temporary sibling is written,
// flushed, and renamed over the destination.
[[nodiscard]] Status publish_snapshot(const std::string& path, std::string_view payload);

}  // namespace po
