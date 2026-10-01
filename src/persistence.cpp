// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "power_observatory/crc32c.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/hashing.hpp"
#include "power_observatory/time.hpp"
#include "power_observatory/version.hpp"

namespace po {
namespace {

constexpr std::array<std::uint8_t, 8> kHeaderMagic = {'P', 'O', 'E', 'V', 'L', 'O', 'G', 0};
constexpr std::size_t kHeaderCrcOffset = 56;
constexpr std::size_t kHeaderCrcCoverage = 56;
constexpr std::size_t kRecordHeaderCrcOffset = 24;
constexpr std::size_t kRecordHeaderCrcCoverage = 24;

// --- little-endian scalar access -------------------------------------------

[[nodiscard]] std::uint16_t read_u16(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[0]) |
                                    static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8));
}

[[nodiscard]] std::uint32_t read_u32(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

[[nodiscard]] std::uint64_t read_u64(const std::uint8_t* bytes) noexcept {
  return static_cast<std::uint64_t>(read_u32(bytes)) |
         (static_cast<std::uint64_t>(read_u32(bytes + 4)) << 32);
}

[[nodiscard]] std::int64_t read_i64(const std::uint8_t* bytes) noexcept {
  return static_cast<std::int64_t>(read_u64(bytes));
}

void write_u16(std::uint8_t* bytes, std::uint16_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value & 0xFFu);
  bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void write_u32(std::uint8_t* bytes, std::uint32_t value) noexcept {
  bytes[0] = static_cast<std::uint8_t>(value & 0xFFu);
  bytes[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
  bytes[2] = static_cast<std::uint8_t>((value >> 16) & 0xFFu);
  bytes[3] = static_cast<std::uint8_t>((value >> 24) & 0xFFu);
}

void write_u64(std::uint8_t* bytes, std::uint64_t value) noexcept {
  write_u32(bytes, static_cast<std::uint32_t>(value & 0xFFFFFFFFull));
  write_u32(bytes + 4, static_cast<std::uint32_t>(value >> 32));
}

void write_i64(std::uint8_t* bytes, std::int64_t value) noexcept {
  write_u64(bytes, static_cast<std::uint64_t>(value));
}

// --- payload writer ---------------------------------------------------------

class Writer {
 public:
  void u8(std::uint8_t value) { buffer_.push_back(value); }

  void u16(std::uint16_t value) {
    buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  }

  void u32(std::uint32_t value) {
    std::uint8_t scratch[4];
    write_u32(scratch, value);
    buffer_.insert(buffer_.end(), scratch, scratch + 4);
  }

  void u64(std::uint64_t value) {
    std::uint8_t scratch[8];
    write_u64(scratch, value);
    buffer_.insert(buffer_.end(), scratch, scratch + 8);
  }

  void i64(std::int64_t value) {
    std::uint8_t scratch[8];
    write_i64(scratch, value);
    buffer_.insert(buffer_.end(), scratch, scratch + 8);
  }

  void raw(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
  }

  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buffer_); }

 private:
  std::vector<std::uint8_t> buffer_;
};

// --- payload reader ---------------------------------------------------------
//
// Every read is bounds checked. A short payload is a refusal, never a
// partially-populated structure.
class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}

  [[nodiscard]] bool remaining(std::size_t count) const noexcept { return cursor_ + count <= size_; }
  [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  [[nodiscard]] bool u8(std::uint8_t& out) noexcept {
    if (!remaining(1)) {
      return false;
    }
    out = data_[cursor_++];
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t& out) noexcept {
    if (!remaining(2)) {
      return false;
    }
    out = read_u16(data_ + cursor_);
    cursor_ += 2;
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t& out) noexcept {
    if (!remaining(4)) {
      return false;
    }
    out = read_u32(data_ + cursor_);
    cursor_ += 4;
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t& out) noexcept {
    if (!remaining(8)) {
      return false;
    }
    out = read_u64(data_ + cursor_);
    cursor_ += 8;
    return true;
  }

  [[nodiscard]] bool i64(std::int64_t& out) noexcept {
    if (!remaining(8)) {
      return false;
    }
    out = read_i64(data_ + cursor_);
    cursor_ += 8;
    return true;
  }

  [[nodiscard]] bool text(std::size_t count, std::string& out) {
    if (!remaining(count)) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(data_ + cursor_), count);
    cursor_ += count;
    return true;
  }

 private:
  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::size_t cursor_{0};
};

[[nodiscard]] bool valid_authority(std::uint8_t raw) noexcept {
  return raw <= static_cast<std::uint8_t>(AuthorityKind::Synthetic);
}

[[nodiscard]] bool valid_entity_kind(std::uint8_t raw) noexcept {
  return raw >= static_cast<std::uint8_t>(EntityKind::Feed) &&
         raw <= static_cast<std::uint8_t>(EntityKind::RedundancyGroup);
}

[[nodiscard]] bool valid_phase(std::uint8_t raw) noexcept {
  return raw <= static_cast<std::uint8_t>(Phase::Unknown);
}

[[nodiscard]] bool valid_measurement_kind(std::uint8_t raw) noexcept {
  return raw <= static_cast<std::uint8_t>(MeasurementKind::Temperature);
}

[[nodiscard]] Result<MeasurementValue> make_value(MeasurementKind kind, QuantityRep raw) {
  switch (kind) {
    case MeasurementKind::ActivePower:
      return MeasurementValue{Power::from_raw(raw)};
    case MeasurementKind::ApparentPower:
      return MeasurementValue{ApparentPower::from_raw(raw)};
    case MeasurementKind::ReactivePower:
      return MeasurementValue{ReactivePower::from_raw(raw)};
    case MeasurementKind::Voltage:
      return MeasurementValue{Voltage::from_raw(raw)};
    case MeasurementKind::Current:
      return MeasurementValue{Current::from_raw(raw)};
    case MeasurementKind::Frequency:
      return MeasurementValue{Frequency::from_raw(raw)};
    case MeasurementKind::PowerFactor:
      return MeasurementValue{Ratio::from_raw(raw)};
    case MeasurementKind::Energy:
      return MeasurementValue{Energy::from_raw(raw)};
    case MeasurementKind::Temperature:
      return MeasurementValue{Temperature::from_raw(raw)};
  }
  return Error(ReasonCode::SchemaViolation, "record carries an unrecognized measurement kind");
}

struct ScanOutcome {
  std::uint64_t committed_length{0};
  std::size_t records_scanned{0};
  ReasonCode tail_reason{ReasonCode::None};
  std::string tail_detail;
  Epoch epoch{};
  std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> records;
};

// A record-shaped hole in the stream is interior corruption only if a
// well-formed record still follows it. This probe answers that question by
// looking for a byte position whose record header and payload both validate
// and whose record index is exactly the one expected next.
[[nodiscard]] bool later_record_valid(const std::vector<std::uint8_t>& bytes, std::uint64_t from,
                                      std::uint64_t minimum_index, const StoreOpenOptions& options) {
  if (bytes.size() < kEvidenceLogRecordHeaderSize) {
    return false;
  }
  const std::uint64_t last = bytes.size() - kEvidenceLogRecordHeaderSize;
  for (std::uint64_t probe = from; probe <= last; ++probe) {
    const std::uint8_t* header = bytes.data() + probe;
    if (read_u32(header) != kEvidenceLogRecordMagic) {
      continue;
    }
    if (read_u16(header + 4) != kEvidenceLogFormatVersion) {
      continue;
    }
    if (Crc32c::compute(header, kRecordHeaderCrcCoverage) != read_u32(header + kRecordHeaderCrcOffset)) {
      continue;
    }
    if (read_u64(header + 12) < minimum_index) {
      continue;
    }
    const std::uint32_t payload_length = read_u32(header + 8);
    if (payload_length > options.max_payload_bytes) {
      continue;
    }
    if (probe + kEvidenceLogRecordHeaderSize + payload_length > bytes.size()) {
      continue;
    }
    if (Crc32c::compute(bytes.data() + probe + kEvidenceLogRecordHeaderSize, payload_length) !=
        read_u32(header + 20)) {
      continue;
    }
    return true;
  }
  return false;
}

[[nodiscard]] Result<ScanOutcome> scan_stream(const std::vector<std::uint8_t>& bytes,
                                              const StoreOpenOptions& options) {
  ScanOutcome outcome;
  if (bytes.size() < kEvidenceLogHeaderSize) {
    return Error(ReasonCode::IntegrityMismatch,
                 "file is shorter than the evidence log header, so it is not an evidence log");
  }
  if (!std::equal(kHeaderMagic.begin(), kHeaderMagic.end(), bytes.begin())) {
    return Error(ReasonCode::IntegrityMismatch, "file does not begin with the evidence log magic");
  }
  const std::uint16_t format_version = read_u16(bytes.data() + 8);
  if (format_version != kEvidenceLogFormatVersion) {
    return Error(ReasonCode::FormatVersionUnsupported,
                 "evidence log format version " + std::to_string(format_version) + " is not supported (this build "
                 "reads version " + std::to_string(kEvidenceLogFormatVersion) + ")");
  }
  const std::uint16_t header_size = read_u16(bytes.data() + 10);
  if (header_size != kEvidenceLogHeaderSize) {
    return Error(ReasonCode::IntegrityMismatch, "evidence log header size field does not match this build");
  }
  if (read_u32(bytes.data() + 12) != kEvidenceLogRecordHeaderSize) {
    return Error(ReasonCode::IntegrityMismatch, "evidence log record header size field does not match this build");
  }
  if (Crc32c::compute(bytes.data(), kHeaderCrcCoverage) != read_u32(bytes.data() + kHeaderCrcOffset)) {
    return Error(ReasonCode::IntegrityMismatch, "evidence log header failed its integrity check");
  }
  outcome.epoch = Epoch(read_u64(bytes.data() + 24));

  std::uint64_t offset = header_size;
  std::uint64_t expected_index = 0;
  while (true) {
    if (offset + kEvidenceLogRecordHeaderSize > bytes.size()) {
      if (offset < bytes.size()) {
        outcome.tail_reason = ReasonCode::TruncatedRecord;
        outcome.tail_detail = "the final record header is incomplete";
      }
      break;
    }
    const std::uint8_t* header = bytes.data() + offset;
    if (read_u32(header) != kEvidenceLogRecordMagic) {
      if (later_record_valid(bytes, offset + 1, expected_index, options)) {
        return Error(ReasonCode::InteriorCorruption,
                     "a damaged record header at byte " + std::to_string(offset) +
                         " is followed by a well-formed record, so the log is corrupt in its interior");
      }
      outcome.tail_reason = ReasonCode::CorruptRecord;
      outcome.tail_detail = "the final record at byte " + std::to_string(offset) +
                            " does not carry the record magic";
      break;
    }
    if (read_u16(header + 4) != kEvidenceLogFormatVersion) {
      return Error(ReasonCode::FormatVersionUnsupported,
                   "record at byte " + std::to_string(offset) + " declares an unsupported format version");
    }
    if (Crc32c::compute(header, kRecordHeaderCrcCoverage) != read_u32(header + kRecordHeaderCrcOffset)) {
      if (later_record_valid(bytes, offset + 1, expected_index, options)) {
        return Error(ReasonCode::InteriorCorruption,
                     "a damaged record header at byte " + std::to_string(offset) +
                         " is followed by a well-formed record, so the log is corrupt in its interior");
      }
      outcome.tail_reason = ReasonCode::CorruptRecord;
      outcome.tail_detail = "the final record header at byte " + std::to_string(offset) +
                            " failed its integrity check";
      break;
    }
    const std::uint32_t payload_length = read_u32(header + 8);
    if (payload_length > options.max_payload_bytes) {
      if (later_record_valid(bytes, offset + 1, expected_index, options)) {
        return Error(ReasonCode::InteriorCorruption,
                     "record at byte " + std::to_string(offset) + " declares " +
                         std::to_string(payload_length) +
                         " payload bytes, beyond the configured bound, and a well-formed record follows it");
      }
      outcome.tail_reason = ReasonCode::CorruptRecord;
      outcome.tail_detail = "the final record declares an implausible payload length";
      break;
    }
    if (offset + kEvidenceLogRecordHeaderSize + payload_length > bytes.size()) {
      outcome.tail_reason = ReasonCode::TruncatedRecord;
      outcome.tail_detail = "the final record is incomplete: its payload is short by " +
                            std::to_string(offset + kEvidenceLogRecordHeaderSize + payload_length - bytes.size()) +
                            " byte(s)";
      break;
    }
    const std::uint64_t record_index = read_u64(header + 12);
    const std::uint8_t* payload = header + kEvidenceLogRecordHeaderSize;
    if (Crc32c::compute(payload, payload_length) != read_u32(header + 20)) {
      // The header is intact, so the damaged record's own index is known and
      // the next record must be one past it.
      if (later_record_valid(bytes, offset + 1, record_index + 1, options)) {
        return Error(ReasonCode::InteriorCorruption,
                     "the payload of the record at byte " + std::to_string(offset) +
                         " failed its integrity check and a well-formed record follows it, so the log is corrupt in "
                         "its interior");
      }
      outcome.tail_reason = ReasonCode::CorruptRecord;
      outcome.tail_detail = "the final record payload at byte " + std::to_string(offset) +
                            " failed its integrity check";
      break;
    }
    if (record_index != expected_index) {
      if (later_record_valid(bytes, offset + 1, expected_index, options)) {
        return Error(ReasonCode::InteriorCorruption,
                     "the record at byte " + std::to_string(offset) + " declares index " +
                         std::to_string(record_index) + " where index " + std::to_string(expected_index) +
                         " was expected and a well-formed record follows it");
      }
      outcome.tail_reason = ReasonCode::CorruptRecord;
      outcome.tail_detail = "the final record declares index " + std::to_string(record_index) + " where index " +
                            std::to_string(expected_index) + " was expected";
      break;
    }

    outcome.records.emplace_back(record_index, std::vector<std::uint8_t>(payload, payload + payload_length));
    offset += kEvidenceLogRecordHeaderSize + payload_length;
    ++expected_index;
    ++outcome.records_scanned;
    if (outcome.records_scanned > options.max_records) {
      return Error(ReasonCode::StorageExhausted,
                   "the evidence log holds more than the configured maximum of " +
                       std::to_string(options.max_records) + " records");
    }
  }

  outcome.committed_length = offset;
  return outcome;
}

[[nodiscard]] std::uint64_t derive_log_id(const std::string& path, Timestamp created_at) noexcept {
  Fnv1a64 hasher;
  hasher.update(path);
  hasher.update_separator('|');
  hasher.update_integral(created_at.unix_nanos());
  return hasher.value();
}

}  // namespace

std::string_view to_string(EvidenceRecordKind kind) noexcept {
  switch (kind) {
    case EvidenceRecordKind::Unknown:
      return "unknown";
    case EvidenceRecordKind::MeasurementBatch:
      return "measurement_batch";
    case EvidenceRecordKind::EpochAdvance:
      return "epoch_advance";
  }
  return "unknown";
}

EvidenceLog::~EvidenceLog() {
  if (handle_.is_open()) {
    static_cast<void>(close());
  }
}

EvidenceLog::EvidenceLog(EvidenceLog&& other) noexcept
    : path_(std::move(other.path_)),
      lock_path_(std::move(other.lock_path_)),
      handle_(std::move(other.handle_)),
      lock_(std::move(other.lock_)),
      recovery_(std::move(other.recovery_)),
      admitted_(std::move(other.admitted_)),
      evidence_cache_(std::move(other.evidence_cache_)),
      evidence_dirty_(other.evidence_dirty_),
      options_(other.options_),
      epoch_(other.epoch_),
      next_record_index_(other.next_record_index_),
      committed_length_(other.committed_length_),
      batch_count_(other.batch_count_),
      committed_mutations_(std::move(other.committed_mutations_)) {}

EvidenceLog& EvidenceLog::operator=(EvidenceLog&& other) noexcept {
  if (this != &other) {
    if (handle_.is_open()) {
      static_cast<void>(close());
    }
    path_ = std::move(other.path_);
    lock_path_ = std::move(other.lock_path_);
    handle_ = std::move(other.handle_);
    lock_ = std::move(other.lock_);
    recovery_ = std::move(other.recovery_);
    admitted_ = std::move(other.admitted_);
    evidence_cache_ = std::move(other.evidence_cache_);
    evidence_dirty_ = other.evidence_dirty_;
    options_ = other.options_;
    epoch_ = other.epoch_;
    next_record_index_ = other.next_record_index_;
    committed_length_ = other.committed_length_;
    batch_count_ = other.batch_count_;
    committed_mutations_ = std::move(other.committed_mutations_);
  }
  return *this;
}

const EvidenceSet& EvidenceLog::evidence() const {
  if (evidence_dirty_) {
    Result<EvidenceSet> rebuilt = EvidenceSet::build(admitted_);
    evidence_cache_ = rebuilt ? std::move(rebuilt).value() : EvidenceSet{};
    evidence_dirty_ = false;
  }
  return evidence_cache_;
}

Result<EvidenceLog> EvidenceLog::open(const std::string& path, Epoch epoch, const StoreOpenOptions& options) {
  if (path.empty()) {
    return Error(ReasonCode::PathInvalid, "evidence log path is empty");
  }

  EvidenceLog log;
  log.path_ = path;
  log.lock_path_ = path + ".lock";
  log.options_ = options;

  Result<FileLock> lock = FileLock::try_acquire_exclusive(log.lock_path_);
  if (!lock) {
    return lock.error();
  }
  log.lock_ = std::move(lock).value();

  if (!file_exists(path)) {
    if (!options.create_if_missing) {
      return Error(ReasonCode::NotFound, "evidence log '" + path + "' does not exist");
    }
    const Status created = log.create_fresh(path, epoch, options);
    if (!created) {
      return created.error();
    }
    return Result<EvidenceLog>(std::move(log));
  }

  const Status recovered = log.recover(options);
  if (!recovered) {
    return recovered.error();
  }

  // An explicit epoch is an assertion of ownership. A caller asking for an
  // older epoch than the log already carries must be refused: honouring it
  // would let a stale owner believe it holds authority it has lost. Epoch zero
  // means "no claim", and adopts whatever the log already says.
  if (epoch.value() != 0 && epoch.value() < log.epoch_.value()) {
    return Error(ReasonCode::EpochMismatch,
                 "the log is at epoch " + std::to_string(log.epoch_.value()) + " but epoch " +
                     std::to_string(epoch.value()) +
                     " was requested; a caller may not reopen at an older epoch");
  }
  if (epoch.value() > log.epoch_.value()) {
    log.epoch_ = epoch;
  }

  return Result<EvidenceLog>(std::move(log));
}

Status EvidenceLog::create_fresh(const std::string& path, Epoch epoch,
                                 [[maybe_unused]] const StoreOpenOptions& options) {
  const std::string directory = parent_directory(path);
  Status status = create_directories(directory);
  if (!status) {
    return status;
  }

  Result<FileHandle> created = FileHandle::create_new(path, true);
  if (!created) {
    return created.error();
  }
  handle_ = std::move(created).value();

  std::array<std::uint8_t, kEvidenceLogHeaderSize> header{};
  std::copy(kHeaderMagic.begin(), kHeaderMagic.end(), header.begin());
  const Timestamp now = SystemClock::instance().wall_now();
  write_u16(header.data() + 8, kEvidenceLogFormatVersion);
  write_u16(header.data() + 10, kEvidenceLogHeaderSize);
  write_u32(header.data() + 12, kEvidenceLogRecordHeaderSize);
  write_i64(header.data() + 16, now.unix_nanos());
  write_u64(header.data() + 24, epoch.value());
  write_u64(header.data() + 32, derive_log_id(path, now));
  write_u32(header.data() + kHeaderCrcOffset, Crc32c::compute(header.data(), kHeaderCrcCoverage));

  status = handle_.append(header.data(), header.size());
  if (status) {
    status = handle_.sync();
  }
  if (!status) {
    static_cast<void>(handle_.close());
    return status;
  }

  committed_length_ = kEvidenceLogHeaderSize;
  next_record_index_ = RecordIndex{};
  batch_count_ = 0;
  epoch_ = epoch;
  recovery_ = RecoveryReport{};
  recovery_.epoch = epoch;
  recovery_.created_new = true;
  recovery_.header_valid = true;
  recovery_.committed_length = kEvidenceLogHeaderSize;
  recovery_.notes.push_back(EvidenceNote{ReasonCode::Ok, Severity::Info,
                                         "evidence log '" + path + "' was created with format version " +
                                             std::to_string(kEvidenceLogFormatVersion),
                                         {}});
  evidence_dirty_ = true;
  return ok_status();
}

Status EvidenceLog::recover(const StoreOpenOptions& options) {
  Result<FileHandle> opened = FileHandle::open_read_write(path_);
  if (!opened) {
    return opened.error();
  }
  handle_ = std::move(opened).value();

  const Result<std::uint64_t> length = handle_.size();
  if (!length) {
    return length.error();
  }
  if (length.value() > options.max_recovery_bytes) {
    return fail(ReasonCode::StorageExhausted,
                "evidence log '" + path_ + "' is " + std::to_string(length.value()) +
                    " bytes, beyond the configured recovery bound of " +
                    std::to_string(options.max_recovery_bytes) + " bytes");
  }
  if (length.value() < kEvidenceLogHeaderSize) {
    return fail(ReasonCode::IntegrityMismatch,
                "evidence log '" + path_ + "' is shorter than its header and cannot be recovered");
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length.value()));
  std::size_t transferred = 0;
  Status status = handle_.read_at(0, bytes.data(), bytes.size(), transferred);
  if (!status) {
    return status;
  }
  if (transferred != bytes.size()) {
    return fail(ReasonCode::ReadFailed, "evidence log '" + path_ + "' could not be read completely");
  }

  Result<ScanOutcome> scanned = scan_stream(bytes, options);
  if (!scanned) {
    return scanned.error();
  }
  ScanOutcome& outcome = scanned.value();

  recovery_ = RecoveryReport{};
  recovery_.created_new = false;
  recovery_.header_valid = true;
  recovery_.records_scanned = outcome.records_scanned;
  recovery_.committed_length = outcome.committed_length;
  recovery_.epoch = outcome.epoch;
  recovery_.tail_reason = outcome.tail_reason;
  recovery_.tail_detail = outcome.tail_detail;
  epoch_ = outcome.epoch;
  next_record_index_ = RecordIndex(outcome.records_scanned);
  committed_length_ = outcome.committed_length;

  if (outcome.committed_length < bytes.size()) {
    const std::uint64_t discarded = static_cast<std::uint64_t>(bytes.size()) - outcome.committed_length;
    recovery_.bytes_discarded = discarded;
    if (options.truncate_torn_tail) {
      status = handle_.truncate_to(outcome.committed_length);
      if (!status) {
        return status;
      }
      status = handle_.sync();
      if (!status) {
        return status;
      }
      recovery_.truncated = true;
      recovery_.notes.push_back(EvidenceNote{
          outcome.tail_reason, severity(outcome.tail_reason),
          "discarded " + std::to_string(discarded) + " trailing byte(s): " + outcome.tail_detail, {}});
    } else {
      recovery_.notes.push_back(EvidenceNote{
          outcome.tail_reason, severity(outcome.tail_reason),
          "left " + std::to_string(discarded) + " trailing byte(s) in place because torn-tail truncation is "
          "disabled: " + outcome.tail_detail, {}});
    }
  }

  // Rebuild the in-memory index from the durable stream.
  std::vector<MutationId> seen_mutations;
  for (const auto& entry : outcome.records) {
    Result<DecodedRecord> decoded = decode_record(entry.second, options);
    if (!decoded) {
      return decoded.error();
    }
    DecodedRecord& record = decoded.value();

    if (record.kind == EvidenceRecordKind::EpochAdvance) {
      if (record.epoch.value() <= epoch_.value()) {
        ++recovery_.records_rejected;
        recovery_.notes.push_back(EvidenceNote{
            ReasonCode::StaleEpoch, severity(ReasonCode::StaleEpoch),
            "record " + std::to_string(entry.first) + " advances the epoch to " +
                std::to_string(record.epoch.value()) + " which is not newer than the current epoch " +
                std::to_string(epoch_.value()) + "; the record is refused", {}});
        continue;
      }
      epoch_ = record.epoch;
      recovery_.epoch = epoch_;
      continue;
    }

    if (record.batch.epoch.value() < epoch_.value()) {
      ++recovery_.records_rejected;
      recovery_.notes.push_back(EvidenceNote{
          ReasonCode::StaleEpoch, severity(ReasonCode::StaleEpoch),
          "batch at record " + std::to_string(entry.first) + " carries epoch " +
              std::to_string(record.batch.epoch.value()) + " which is older than the log epoch " +
              std::to_string(epoch_.value()) + "; the batch is refused as a replay", {}});
      continue;
    }
    if (record.batch.epoch.value() > epoch_.value()) {
      epoch_ = record.batch.epoch;
      recovery_.epoch = epoch_;
    }

    if (std::find(seen_mutations.begin(), seen_mutations.end(), record.batch.mutation) != seen_mutations.end()) {
      ++recovery_.records_skipped_replay;
      recovery_.notes.push_back(EvidenceNote{
          ReasonCode::IdempotentReplay, severity(ReasonCode::IdempotentReplay),
          "batch at record " + std::to_string(entry.first) + " repeats mutation " +
              std::to_string(record.batch.mutation.value()) + " which is already durable; it is not applied twice",
          {}});
      continue;
    }
    seen_mutations.push_back(record.batch.mutation);

    for (const Measurement& measurement : record.batch.measurements) {
      admitted_.push_back(measurement);
      if (measurement.provenance.generation > recovery_.highest_generation) {
        recovery_.highest_generation = measurement.provenance.generation;
      }
    }
    ++recovery_.records_applied;
    ++batch_count_;
  }

  committed_mutations_ = std::move(seen_mutations);
  evidence_dirty_ = true;

  // Reopen for appending so that writes are atomically appended at the end of
  // the file and never land on top of what recovery read.
  const Status closed = handle_.close();
  if (!closed) {
    return closed;
  }
  Result<FileHandle> append_handle = FileHandle::open_append(path_);
  if (!append_handle) {
    return append_handle.error();
  }
  handle_ = std::move(append_handle).value();
  return ok_status();
}

Result<EvidenceLog::Loaded> EvidenceLog::load(const std::string& path, const StoreOpenOptions& options) {
  Result<FileLock> lock = FileLock::try_acquire_shared(path + ".lock");
  if (!lock) {
    if (lock.error().code() != ReasonCode::WriterLockHeld) {
      return lock.error();
    }
    // A shared lock cannot be taken while a writer holds the exclusive lock.
    return Error(ReasonCode::WriterLockHeld,
                 "evidence log '" + path + "' is currently owned by a writer; a read-only load cannot proceed");
  }
  // The shared lock is held for the whole read and released when it leaves
  // scope. It exists to exclude a concurrent writer, not to be inspected.
  [[maybe_unused]] const FileLock held = std::move(lock).value();

  Result<FileHandle> handle = FileHandle::open_read(path);
  if (!handle) {
    return handle.error();
  }
  const Result<std::uint64_t> length = handle.value().size();
  if (!length) {
    return length.error();
  }
  if (length.value() > options.max_recovery_bytes) {
    return Error(ReasonCode::StorageExhausted,
                 "evidence log '" + path + "' is larger than the configured recovery bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length.value()));
  if (!bytes.empty()) {
    std::size_t transferred = 0;
    const Status status = handle.value().read_at(0, bytes.data(), bytes.size(), transferred);
    if (!status) {
      return status.error();
    }
    if (transferred != bytes.size()) {
      return Error(ReasonCode::ReadFailed, "evidence log '" + path + "' could not be read completely");
    }
  }

  Result<ScanOutcome> scanned = scan_stream(bytes, options);
  if (!scanned) {
    return scanned.error();
  }
  ScanOutcome& outcome = scanned.value();

  Loaded loaded;
  loaded.report.header_valid = true;
  loaded.report.records_scanned = outcome.records_scanned;
  loaded.report.committed_length = outcome.committed_length;
  loaded.report.epoch = outcome.epoch;
  loaded.report.tail_reason = outcome.tail_reason;
  loaded.report.tail_detail = outcome.tail_detail;
  loaded.report.truncated = false;
  loaded.report.bytes_discarded = static_cast<std::uint64_t>(bytes.size()) - outcome.committed_length;
  if (loaded.report.bytes_discarded > 0) {
    loaded.report.notes.push_back(EvidenceNote{
        outcome.tail_reason, severity(outcome.tail_reason),
        "a read-only load observed " + std::to_string(loaded.report.bytes_discarded) +
            " trailing byte(s) and left them in place: " + outcome.tail_detail, {}});
  }
  loaded.epoch = outcome.epoch;
  loaded.next_record_index = RecordIndex(outcome.records_scanned);

  std::vector<Measurement> admitted;
  std::vector<MutationId> seen_mutations;
  Epoch epoch = outcome.epoch;
  for (const auto& entry : outcome.records) {
    Result<DecodedRecord> decoded = decode_record(entry.second, options);
    if (!decoded) {
      return decoded.error();
    }
    DecodedRecord& record = decoded.value();
    if (record.kind == EvidenceRecordKind::EpochAdvance) {
      if (record.epoch.value() <= epoch.value()) {
        ++loaded.report.records_rejected;
        continue;
      }
      epoch = record.epoch;
      loaded.epoch = epoch;
      continue;
    }
    if (record.batch.epoch.value() < epoch.value()) {
      ++loaded.report.records_rejected;
      continue;
    }
    if (record.batch.epoch.value() > epoch.value()) {
      epoch = record.batch.epoch;
      loaded.epoch = epoch;
    }
    if (std::find(seen_mutations.begin(), seen_mutations.end(), record.batch.mutation) != seen_mutations.end()) {
      ++loaded.report.records_skipped_replay;
      continue;
    }
    seen_mutations.push_back(record.batch.mutation);
    ++loaded.report.records_applied;
    for (const Measurement& measurement : record.batch.measurements) {
      admitted.push_back(measurement);
      if (measurement.provenance.generation > loaded.report.highest_generation) {
        loaded.report.highest_generation = measurement.provenance.generation;
      }
    }
  }

  Result<EvidenceSet> built = EvidenceSet::build(std::move(admitted));
  if (!built) {
    return built.error();
  }
  loaded.evidence = std::move(built).value();
  return loaded;
}

Result<RecoveryReport> EvidenceLog::inspect(const std::string& path, const StoreOpenOptions& options) {
  Result<FileHandle> handle = FileHandle::open_read(path);
  if (!handle) {
    return handle.error();
  }
  const Result<std::uint64_t> length = handle.value().size();
  if (!length) {
    return length.error();
  }
  if (length.value() > options.max_recovery_bytes) {
    return Error(ReasonCode::StorageExhausted,
                 "evidence log '" + path + "' is larger than the configured recovery bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length.value()));
  if (!bytes.empty()) {
    std::size_t transferred = 0;
    const Status status = handle.value().read_at(0, bytes.data(), bytes.size(), transferred);
    if (!status) {
      return status.error();
    }
    if (transferred != bytes.size()) {
      return Error(ReasonCode::ReadFailed, "evidence log '" + path + "' could not be read completely");
    }
  }

  Result<ScanOutcome> scanned = scan_stream(bytes, options);
  if (!scanned) {
    return scanned.error();
  }
  ScanOutcome& outcome = scanned.value();

  RecoveryReport report;
  report.header_valid = true;
  report.records_scanned = outcome.records_scanned;
  report.committed_length = outcome.committed_length;
  report.epoch = outcome.epoch;
  report.tail_reason = outcome.tail_reason;
  report.tail_detail = outcome.tail_detail;
  report.bytes_discarded = static_cast<std::uint64_t>(bytes.size()) - outcome.committed_length;

  std::vector<MutationId> seen_mutations;
  for (const auto& entry : outcome.records) {
    Result<DecodedRecord> decoded = decode_record(entry.second, options);
    if (!decoded) {
      return decoded.error();
    }
    DecodedRecord& record = decoded.value();
    if (record.kind == EvidenceRecordKind::EpochAdvance) {
      if (record.epoch.value() <= report.epoch.value()) {
        ++report.records_rejected;
        continue;
      }
      report.epoch = record.epoch;
      continue;
    }
    if (record.batch.epoch.value() < report.epoch.value()) {
      ++report.records_rejected;
      continue;
    }
    if (std::find(seen_mutations.begin(), seen_mutations.end(), record.batch.mutation) != seen_mutations.end()) {
      ++report.records_skipped_replay;
      continue;
    }
    seen_mutations.push_back(record.batch.mutation);
    ++report.records_applied;
    for (const Measurement& measurement : record.batch.measurements) {
      if (measurement.provenance.generation > report.highest_generation) {
        report.highest_generation = measurement.provenance.generation;
      }
    }
  }
  return report;
}

Result<std::vector<std::uint8_t>> EvidenceLog::encode_batch(const EvidenceBatch& batch,
                                                            const StoreOpenOptions& options) {
  if (batch.source.empty()) {
    return Error(ReasonCode::SchemaViolation, "evidence batch carries no source identifier");
  }
  if (batch.source.size() > options.max_identifier_bytes) {
    return Error(ReasonCode::ValueOutOfRange, "evidence batch source identifier is longer than the configured bound");
  }
  if (batch.measurements.size() > options.max_measurements_per_batch) {
    return Error(ReasonCode::ValueOutOfRange,
                 "evidence batch holds " + std::to_string(batch.measurements.size()) +
                     " measurements, beyond the configured bound of " +
                     std::to_string(options.max_measurements_per_batch));
  }

  Writer writer;
  writer.u8(static_cast<std::uint8_t>(EvidenceRecordKind::MeasurementBatch));
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.u64(batch.mutation.value());
  writer.u64(batch.attempt.value());
  writer.u64(batch.generation.value());
  writer.u64(batch.epoch.value());
  writer.u64(batch.first_sequence.value());
  writer.i64(batch.recorded_at.unix_nanos());
  writer.u8(static_cast<std::uint8_t>(batch.authority));
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.u16(static_cast<std::uint16_t>(batch.source.size()));
  writer.raw(batch.source.value().data(), batch.source.size());
  writer.u32(static_cast<std::uint32_t>(batch.measurements.size()));

  for (const Measurement& measurement : batch.measurements) {
    if (!measurement.valid()) {
      return Error(ReasonCode::SchemaViolation, "evidence batch holds a measurement without an identifier or entity");
    }
    if (measurement.entity.id().size() > options.max_identifier_bytes) {
      return Error(ReasonCode::ValueOutOfRange, "measurement entity identifier is longer than the configured bound");
    }
    writer.u64(measurement.id.value());
    writer.u8(static_cast<std::uint8_t>(measurement.entity.kind()));
    writer.u8(static_cast<std::uint8_t>(measurement.phase));
    writer.u8(static_cast<std::uint8_t>(measurement.kind()));
    writer.u8(0);
    writer.i64(raw_value(measurement.value));
    writer.u8(measurement.provenance.source_time.has_value() ? 1 : 0);
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    writer.i64(measurement.provenance.source_time.has_value()
                   ? measurement.provenance.source_time->unix_nanos()
                   : 0);
    writer.i64(measurement.provenance.received_time.unix_nanos());
    writer.u64(measurement.provenance.sequence.value());
    writer.u64(measurement.provenance.generation.value());
    writer.u8(static_cast<std::uint8_t>(measurement.provenance.authority));
    writer.u8(0);
    writer.u8(0);
    writer.u8(0);
    writer.u16(static_cast<std::uint16_t>(measurement.entity.id().size()));
    writer.raw(measurement.entity.id().data(), measurement.entity.id().size());
  }
  return writer.take();
}

Result<std::vector<std::uint8_t>> EvidenceLog::encode_epoch_advance(Epoch epoch, std::string_view reason,
                                                                   const StoreOpenOptions& options) {
  if (reason.size() > options.max_identifier_bytes) {
    return Error(ReasonCode::ValueOutOfRange, "epoch advance reason is longer than the configured bound");
  }
  Writer writer;
  writer.u8(static_cast<std::uint8_t>(EvidenceRecordKind::EpochAdvance));
  writer.u8(0);
  writer.u8(0);
  writer.u8(0);
  writer.u64(epoch.value());
  writer.u16(static_cast<std::uint16_t>(reason.size()));
  writer.raw(reason.data(), reason.size());
  return writer.take();
}

Result<EvidenceLog::DecodedRecord> EvidenceLog::decode_record(const std::vector<std::uint8_t>& payload,
                                                             const StoreOpenOptions& options) {
  DecodedRecord record;
  Reader reader(payload.data(), payload.size());

  std::uint8_t kind = 0;
  if (!reader.u8(kind)) {
    return Error(ReasonCode::CorruptRecord, "record payload is empty");
  }
  if (kind != static_cast<std::uint8_t>(EvidenceRecordKind::MeasurementBatch) &&
      kind != static_cast<std::uint8_t>(EvidenceRecordKind::EpochAdvance)) {
    return Error(ReasonCode::SchemaViolation,
                 "record payload declares unrecognized kind " + std::to_string(kind));
  }
  record.kind = static_cast<EvidenceRecordKind>(kind);

  std::uint8_t reserved = 0;
  for (int index = 0; index < 3; ++index) {
    if (!reader.u8(reserved)) {
      return Error(ReasonCode::CorruptRecord, "record payload is truncated in its header");
    }
  }

  if (record.kind == EvidenceRecordKind::EpochAdvance) {
    std::uint64_t epoch_value = 0;
    std::uint16_t reason_length = 0;
    if (!reader.u64(epoch_value) || !reader.u16(reason_length) || !reader.text(reason_length, record.epoch_reason)) {
      return Error(ReasonCode::CorruptRecord, "epoch advance record is truncated");
    }
    if (reader.cursor() != reader.size()) {
      return Error(ReasonCode::CorruptRecord, "epoch advance record carries trailing bytes");
    }
    record.epoch = Epoch(epoch_value);
    return record;
  }

  std::uint64_t mutation = 0;
  std::uint64_t attempt = 0;
  std::uint64_t generation = 0;
  std::uint64_t epoch_value = 0;
  std::uint64_t first_sequence = 0;
  std::int64_t recorded_at = 0;
  std::uint8_t authority = 0;
  std::uint16_t source_length = 0;
  std::uint32_t count = 0;

  if (!reader.u64(mutation) || !reader.u64(attempt) || !reader.u64(generation) || !reader.u64(epoch_value) ||
      !reader.u64(first_sequence) || !reader.i64(recorded_at)) {
    return Error(ReasonCode::CorruptRecord, "measurement batch record is truncated in its header");
  }
  if (!reader.u8(authority)) {
    return Error(ReasonCode::CorruptRecord, "measurement batch record is truncated before its authority");
  }
  for (int index = 0; index < 3; ++index) {
    if (!reader.u8(reserved)) {
      return Error(ReasonCode::CorruptRecord, "measurement batch record is truncated in its header");
    }
  }
  if (!valid_authority(authority)) {
    return Error(ReasonCode::SchemaViolation, "measurement batch record declares an unrecognized authority");
  }
  if (!reader.u16(source_length) || source_length == 0 || source_length > options.max_identifier_bytes) {
    return Error(ReasonCode::SchemaViolation, "measurement batch record declares an implausible source length");
  }
  std::string source;
  if (!reader.text(source_length, source)) {
    return Error(ReasonCode::CorruptRecord, "measurement batch record is truncated inside its source identifier");
  }
  if (!reader.u32(count) || count > options.max_measurements_per_batch) {
    return Error(ReasonCode::SchemaViolation,
                 "measurement batch record declares " + std::to_string(count) +
                     " measurements, beyond the configured bound");
  }

  record.batch.source = SourceId(std::move(source));
  record.batch.authority = static_cast<AuthorityKind>(authority);
  record.batch.generation = Generation(generation);
  record.batch.epoch = Epoch(epoch_value);
  record.batch.first_sequence = Sequence(first_sequence);
  record.batch.mutation = MutationId(mutation);
  record.batch.attempt = AttemptId(attempt);
  record.batch.recorded_at = Timestamp::from_unix_nanos(recorded_at);
  record.batch.measurements.reserve(count);

  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint64_t id = 0;
    std::uint8_t entity_kind = 0;
    std::uint8_t phase = 0;
    std::uint8_t value_kind = 0;
    std::int64_t value_raw = 0;
    std::uint8_t has_source_time = 0;
    std::int64_t source_time = 0;
    std::int64_t received_time = 0;
    std::uint64_t sequence = 0;
    std::uint64_t measurement_generation = 0;
    std::uint8_t measurement_authority = 0;
    std::uint16_t entity_length = 0;

    if (!reader.u64(id) || !reader.u8(entity_kind) || !reader.u8(phase) || !reader.u8(value_kind)) {
      return Error(ReasonCode::CorruptRecord, "measurement record is truncated in its key");
    }
    if (!reader.u8(reserved) || !reader.i64(value_raw) || !reader.u8(has_source_time)) {
      return Error(ReasonCode::CorruptRecord, "measurement record is truncated in its value");
    }
    for (int pad = 0; pad < 3; ++pad) {
      if (!reader.u8(reserved)) {
        return Error(ReasonCode::CorruptRecord, "measurement record is truncated in its value");
      }
    }
    if (!reader.i64(source_time) || !reader.i64(received_time) || !reader.u64(sequence) ||
        !reader.u64(measurement_generation)) {
      return Error(ReasonCode::CorruptRecord, "measurement record is truncated in its provenance");
    }
    if (!reader.u8(measurement_authority)) {
      return Error(ReasonCode::CorruptRecord, "measurement record is truncated before its authority");
    }
    for (int pad = 0; pad < 3; ++pad) {
      if (!reader.u8(reserved)) {
        return Error(ReasonCode::CorruptRecord, "measurement record is truncated before its authority");
      }
    }
    if (!reader.u16(entity_length) || entity_length == 0 || entity_length > options.max_identifier_bytes) {
      return Error(ReasonCode::SchemaViolation, "measurement record declares an implausible entity length");
    }
    std::string entity_id;
    if (!reader.text(entity_length, entity_id)) {
      return Error(ReasonCode::CorruptRecord, "measurement record is truncated inside its entity identifier");
    }

    if (id == 0) {
      return Error(ReasonCode::SchemaViolation, "measurement record declares identifier zero");
    }
    if (!valid_entity_kind(entity_kind)) {
      return Error(ReasonCode::SchemaViolation, "measurement record declares an unrecognized entity kind");
    }
    if (!valid_phase(phase)) {
      return Error(ReasonCode::SchemaViolation, "measurement record declares an unrecognized phase");
    }
    if (!valid_measurement_kind(value_kind)) {
      return Error(ReasonCode::SchemaViolation, "measurement record declares an unrecognized measurement kind");
    }
    if (!valid_authority(measurement_authority)) {
      return Error(ReasonCode::SchemaViolation, "measurement record declares an unrecognized authority");
    }

    Result<MeasurementValue> value = make_value(static_cast<MeasurementKind>(value_kind), value_raw);
    if (!value) {
      return value.error();
    }

    Measurement measurement;
    measurement.id = MeasurementId(id);
    measurement.entity = EntityRef::of(static_cast<EntityKind>(entity_kind), std::move(entity_id));
    measurement.phase = static_cast<Phase>(phase);
    measurement.value = std::move(value).value();
    measurement.provenance = make_recovered_provenance(
        record.batch.source, static_cast<AuthorityKind>(measurement_authority),
        Generation(measurement_generation), Epoch(epoch_value), Sequence(sequence),
        has_source_time != 0 ? std::optional<Timestamp>(Timestamp::from_unix_nanos(source_time)) : std::nullopt,
        Timestamp::from_unix_nanos(received_time));
    record.batch.measurements.push_back(std::move(measurement));
  }

  if (reader.cursor() != reader.size()) {
    return Error(ReasonCode::CorruptRecord, "measurement batch record carries trailing bytes");
  }
  return record;
}

Status EvidenceLog::append_record(const std::vector<std::uint8_t>& record) {
  std::array<std::uint8_t, kEvidenceLogRecordHeaderSize> header{};
  write_u32(header.data(), kEvidenceLogRecordMagic);
  write_u16(header.data() + 4, kEvidenceLogFormatVersion);
  write_u16(header.data() + 6, 0);

  const auto payload_length = static_cast<std::uint64_t>(record.size());
  if (payload_length == 0 || payload_length > 0xFFFFFFFFull) {
    return fail(ReasonCode::ValueOutOfRange, "record payload length is outside the representable range");
  }
  write_u32(header.data() + 8, static_cast<std::uint32_t>(payload_length));
  write_u64(header.data() + 12, next_record_index_.value());
  write_u32(header.data() + 20, Crc32c::compute(record.data(), record.size()));
  write_u32(header.data() + kRecordHeaderCrcOffset, Crc32c::compute(header.data(), kRecordHeaderCrcCoverage));

  Status status = handle_.append(header.data(), header.size());
  if (!status) {
    return status;
  }
  status = handle_.append(record.data(), record.size());
  if (!status) {
    return status;
  }
  return ok_status();
}

Status EvidenceLog::append(const EvidenceBatch& batch) {
  if (!handle_.is_open()) {
    return fail(ReasonCode::NotStarted, "evidence log '" + path_ + "' is not open");
  }
  if (!batch.source.empty() && batch.epoch.value() < epoch_.value()) {
    return fail(ReasonCode::StaleEpoch,
                "batch carries epoch " + std::to_string(batch.epoch.value()) + " which is older than the log epoch " +
                    std::to_string(epoch_.value()) + "; the batch is refused as a replay");
  }
  if (batch.epoch.value() > epoch_.value()) {
    return fail(ReasonCode::EpochMismatch,
                "batch carries epoch " + std::to_string(batch.epoch.value()) + " which is newer than the log epoch " +
                    std::to_string(epoch_.value()) + "; advance the epoch before admitting this batch");
  }
  if (std::find(committed_mutations_.begin(), committed_mutations_.end(), batch.mutation) !=
      committed_mutations_.end()) {
    return fail(ReasonCode::IdempotentReplay,
                "mutation " + std::to_string(batch.mutation.value()) + " is already durable; the retry is ignored");
  }

  Result<std::vector<std::uint8_t>> encoded = encode_batch(batch, options_);
  if (!encoded) {
    return encoded.error();
  }

  const std::uint64_t record_length =
      static_cast<std::uint64_t>(kEvidenceLogRecordHeaderSize) + encoded.value().size();
  Status status = append_record(encoded.value());
  if (!status) {
    return fail(ReasonCode::CommitFailed,
                "record " + std::to_string(next_record_index_.value()) + " was not written completely: " +
                    status.detail());
  }
  // Commit point: after this call the record is durable and observable to a
  // later recovery. In-memory state is only advanced once the flush succeeded.
  status = handle_.sync();
  if (!status) {
    return fail(ReasonCode::CommitFailed,
                "record " + std::to_string(next_record_index_.value()) +
                    " was written but could not be flushed to stable storage: " + status.detail());
  }

  committed_length_ += record_length;
  next_record_index_ = RecordIndex(next_record_index_.value() + 1);
  committed_mutations_.push_back(batch.mutation);
  for (const Measurement& measurement : batch.measurements) {
    admitted_.push_back(measurement);
    if (measurement.provenance.generation > recovery_.highest_generation) {
      recovery_.highest_generation = measurement.provenance.generation;
    }
  }
  ++batch_count_;
  evidence_dirty_ = true;
  return ok_status();
}

Status EvidenceLog::advance_epoch(Epoch epoch, std::string reason) {
  if (!handle_.is_open()) {
    return fail(ReasonCode::NotStarted, "evidence log '" + path_ + "' is not open");
  }
  if (epoch.value() <= epoch_.value()) {
    return fail(ReasonCode::EpochRegression,
                "epoch " + std::to_string(epoch.value()) + " is not newer than the current epoch " +
                    std::to_string(epoch_.value()));
  }
  Result<std::vector<std::uint8_t>> encoded = encode_epoch_advance(epoch, reason, options_);
  if (!encoded) {
    return encoded.error();
  }
  Status status = append_record(encoded.value());
  if (!status) {
    return fail(ReasonCode::CommitFailed, "epoch advance record could not be written: " + status.detail());
  }
  status = handle_.sync();
  if (!status) {
    return fail(ReasonCode::CommitFailed, "epoch advance record could not be flushed: " + status.detail());
  }
  committed_length_ += static_cast<std::uint64_t>(kEvidenceLogRecordHeaderSize) + encoded.value().size();
  next_record_index_ = RecordIndex(next_record_index_.value() + 1);
  epoch_ = epoch;
  recovery_.epoch = epoch;
  recovery_.notes.push_back(EvidenceNote{ReasonCode::Ok, Severity::Info,
                                         "epoch advanced to " + std::to_string(epoch.value()) + ": " + reason, {}});
  return ok_status();
}

Status EvidenceLog::sync() {
  if (!handle_.is_open()) {
    return fail(ReasonCode::NotStarted, "evidence log '" + path_ + "' is not open");
  }
  return handle_.sync();
}

Status EvidenceLog::close() {
  if (!handle_.is_open()) {
    return ok_status();
  }
  const Status status = handle_.close();
  lock_.release();
  return status;
}

Status publish_snapshot(const std::string& path, std::string_view payload) {
  const std::string directory = parent_directory(path);
  Status status = create_directories(directory);
  if (!status) {
    return status;
  }
  return write_file_atomically(path, payload.data(), payload.size());
}

}  // namespace po
