// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace po {

// Severity of a single explanatory reason. Severity is part of the published
// contract: consumers may gate on it, so values are stable and never reused.
enum class Severity : std::uint8_t {
  Info = 0,
  Notice = 1,
  Warning = 2,
  Error = 3,
  Critical = 4,
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

// Every reason this runtime can attach to an outcome. Numeric values are
// grouped by category and are part of the wire contract: never renumber,
// never reuse a retired value. Additions must take unused numbers inside the
// matching category block.
#define PO_REASON_CODES(X)                                                     \
  /* ---- 0: absence of a reason ---- */                                       \
  X(None, 0, Info)                                                             \
  /* ---- 100..199: informational, success, nominal ---- */                     \
  X(Ok, 100, Info)                                                             \
  X(ObservedEvidence, 101, Info)                                               \
  X(DerivedEvidence, 102, Info)                                                \
  X(WithinTolerance, 103, Info)                                                \
  X(ReserveAdequate, 104, Info)                                                \
  X(FailoverReady, 105, Info)                                                  \
  X(NoConstraint, 106, Info)                                                   \
  X(NoLossDetected, 107, Info)                                                 \
  X(FlowEstablished, 108, Info)                                                \
  X(QualityNominal, 109, Info)                                                 \
  X(IdempotentReplay, 110, Info)                                               \
  X(HistoryRetained, 111, Info)                                                \
  /* ---- 200..299: unknown, missing, indeterminate ---- */                     \
  X(UnknownSource, 200, Notice)                                                \
  X(UnknownEntity, 201, Notice)                                                \
  X(MissingMeasurement, 202, Notice)                                           \
  X(NoEvidence, 203, Notice)                                                   \
  X(CapacityUnknown, 204, Notice)                                              \
  X(TopologyUnknown, 205, Notice)                                              \
  X(LossModelUnavailable, 206, Notice)                                         \
  X(NotMeasured, 207, Notice)                                                  \
  X(ReserveUnknown, 208, Notice)                                               \
  X(FlowUnknown, 209, Notice)                                                  \
  X(FailoverUnknown, 210, Notice)                                              \
  X(Indeterminate, 211, Notice)                                                \
  /* ---- 300..399: freshness and time ---- */                                  \
  X(StaleEvidence, 300, Warning)                                               \
  X(ExpiredEvidence, 301, Warning)                                             \
  X(FreshnessUnknown, 302, Warning)                                            \
  X(RecoveredNotFresh, 303, Warning)                                           \
  X(MonotonicAnchorLost, 304, Warning)                                         \
  X(ClockSkewExceeded, 305, Warning)                                           \
  X(FutureTimestamp, 306, Warning)                                             \
  X(SourceTimeMissing, 307, Warning)                                           \
  X(AgingEvidence, 308, Info)                                                  \
  /* ---- 400..499: conflict and disagreement ---- */                           \
  X(ConflictingMeters, 400, Warning)                                           \
  X(ConflictingTopology, 401, Warning)                                         \
  X(DuplicateSample, 402, Info)                                                \
  X(ReorderedSample, 403, Info)                                                \
  X(SourceDisagreement, 404, Warning)                                          \
  X(GenerationMismatch, 405, Warning)                                          \
  X(EpochMismatch, 406, Warning)                                               \
  X(ImbalanceUnattributed, 407, Warning)                                       \
  X(SplitBrainEvidence, 408, Error)                                            \
  /* ---- 500..599: numeric range and checked arithmetic ---- */                \
  X(ValueOutOfRange, 500, Error)                                               \
  X(ArithmeticOverflow, 501, Error)                                            \
  X(ArithmeticUnderflow, 502, Error)                                           \
  X(DivisionByZero, 503, Error)                                                \
  X(NegativeQuantity, 504, Error)                                              \
  X(NumericExtreme, 505, Warning)                                              \
  X(FrequencyOutOfRange, 506, Warning)                                         \
  X(VoltageOutOfRange, 507, Warning)                                           \
  X(PowerFactorOutOfRange, 508, Warning)                                       \
  X(PhaseImbalanceExceeded, 509, Warning)                                      \
  X(UnsupportedQuantity, 510, Notice)                                          \
  /* ---- 600..699: reserve and capacity ---- */                                \
  X(ReserveNegative, 600, Error)                                               \
  X(ReserveExhausted, 601, Error)                                              \
  X(ReserveBelowFloor, 602, Warning)                                           \
  X(RedundancyLost, 603, Error)                                                \
  X(CapacityDerated, 604, Warning)                                             \
  X(LoadExceedsCapacity, 605, Error)                                                 X(LossExceeded, 606, Warning)                                           \
  /* ---- 700..799: failover readiness ---- */                                  \
  X(FailoverBlocked, 700, Error)                                               \
  X(TransferWindowExceeded, 701, Warning)                                      \
  X(SyncNotEstablished, 702, Warning)                                          \
  X(PeerPathDead, 703, Error)                                                  \
  X(FailoverDegraded, 704, Warning)                                            \
  X(FailoverUnsupported, 705, Notice)                                          \
  X(InsufficientAutonomy, 706, Warning)                                        \
  /* ---- 800..899: authority boundary and provenance ---- */                   \
  X(AuthorityViolation, 800, Critical)                                         \
  X(AuthorityNotOwned, 801, Error)                                             \
  X(ActuationRefused, 802, Error)                                              \
  X(UnsupportedCapability, 803, Notice)                                        \
  X(StaleGeneration, 804, Warning)                                             \
  X(StaleEpoch, 805, Warning)                                                  \
  X(ReplayRejected, 806, Warning)                                              \
  X(ConfiguredNotObserved, 807, Warning)                                       \
  X(AcknowledgementNotEffect, 808, Warning)                                    \
  /* ---- 900..999: persistence, integrity, recovery ---- */                    \
  X(CorruptRecord, 900, Error)                                                 \
  X(TornTail, 901, Warning)                                                    \
  X(InteriorCorruption, 902, Critical)                                         \
  X(IntegrityMismatch, 903, Critical)                                          \
  X(FormatVersionUnsupported, 904, Error)                                      \
  X(WriterLockHeld, 905, Error)                                                \
  X(CommitFailed, 906, Error)                                                  \
  X(NotFound, 907, Notice)                                                     \
  X(OpenFailed, 908, Error)                                                    \
  X(EpochRegression, 909, Error)                                               \
  X(StorageExhausted, 910, Critical)                                           \
  X(ReadFailed, 911, Error)                                                    \
  X(WriteFailed, 912, Error)                                                   \
  X(StaleSnapshot, 913, Warning)                                               \
  X(LockFailed, 914, Error)                                                    \
  X(PathInvalid, 915, Error)                                                   \
  X(TruncatedRecord, 916, Error)                                               \
  X(SnapshotMismatch, 917, Error)                                              \
  /* ---- 1000..1099: lifecycle and concurrency ---- */                         \
  X(NotStarted, 1000, Error)                                                   \
  X(AlreadyStarted, 1001, Error)                                               \
  X(ShuttingDown, 1002, Notice)                                                \
  X(AlreadyStopped, 1003, Notice)                                              \
  X(Cancelled, 1004, Notice)                                                   \
  X(InvalidArgument, 1005, Error)                                              \
  X(InternalInvariant, 1006, Critical)                                         \
  X(OperationTimedOut, 1007, Error)                                            \
  X(Busy, 1008, Notice)                                                        \
  X(WorkerFailed, 1009, Critical)                                              \
  X(QueueFull, 1010, Error)                                                    \
  X(OwnerThreadMismatch, 1011, Error)                                          \
  X(ConcurrentPublication, 1012, Error)                                        \
  /* ---- 1100..1199: parsing, schema, io ---- */                               \
  X(ParseError, 1100, Error)                                                   \
  X(UnsupportedToken, 1101, Error)                                             \
  X(SchemaViolation, 1102, Error)                                              \
  X(IoError, 1103, Error)                                                      \
  X(UnexpectedEndOfInput, 1104, Error)

enum class ReasonCode : std::uint16_t {
#define PO_REASON_ENUM(name, value, severity) name = value,
  PO_REASON_CODES(PO_REASON_ENUM)
#undef PO_REASON_ENUM
};

// Stable snake_case identifier used by every serialized surface.
[[nodiscard]] std::string_view to_string(ReasonCode code) noexcept;

// Declared severity. Kept in one table with the code list so the two can never
// drift apart.
[[nodiscard]] Severity severity(ReasonCode code) noexcept;

// Coarse category used for grouping in reports.
enum class ReasonCategory : std::uint8_t {
  None = 0,
  Informational = 1,
  Unknown = 2,
  Freshness = 3,
  Conflict = 4,
  Numeric = 5,
  Reserve = 6,
  Failover = 7,
  Authority = 8,
  Persistence = 9,
  Lifecycle = 10,
  Parsing = 11,
};

[[nodiscard]] ReasonCategory category(ReasonCode code) noexcept;
[[nodiscard]] std::string_view to_string(ReasonCategory category) noexcept;

// Number of distinct reason codes; useful for exhaustive tests.
[[nodiscard]] std::size_t reason_code_count() noexcept;
// The i-th reason code in ascending numeric order. Precondition: i < count.
[[nodiscard]] ReasonCode reason_code_at(std::size_t index) noexcept;

}  // namespace po
