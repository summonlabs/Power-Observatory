// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace po {

// StrongValue wraps a representation in a distinct type so that two identities
// of different kinds can never be interchanged by accident. The tag type is
// never defined; it exists only to make the specialization unique.
template <class Tag, class T>
class StrongValue {
 public:
  using value_type = T;
  using tag_type = Tag;

  constexpr StrongValue() = default;
  constexpr explicit StrongValue(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
      : value_(std::move(value)) {}

  [[nodiscard]] constexpr const T& value() const& noexcept { return value_; }
  [[nodiscard]] constexpr T& value() & noexcept { return value_; }
  [[nodiscard]] constexpr T&& value() && noexcept { return std::move(value_); }

  [[nodiscard]] constexpr bool empty() const noexcept
    requires requires(const T& candidate) { candidate.empty(); }
  {
    return value_.empty();
  }

  [[nodiscard]] constexpr std::size_t size() const noexcept
    requires requires(const T& candidate) { candidate.size(); }
  {
    return value_.size();
  }

  [[nodiscard]] constexpr std::string_view view() const noexcept
    requires requires(const T& candidate) { std::string_view(candidate); }
  {
    return std::string_view(value_);
  }

  friend constexpr bool operator==(const StrongValue&, const StrongValue&) noexcept = default;
  friend constexpr auto operator<=>(const StrongValue&, const StrongValue&) noexcept = default;

 private:
  T value_{};
};

// Monotonic counters. Counters never wrap: successor() reports exhaustion
// instead of returning a value that has already been used.
template <class Tag>
class Counter {
 public:
  using rep = std::uint64_t;
  using tag_type = Tag;

  constexpr Counter() = default;
  constexpr explicit Counter(rep value) noexcept : value_(value) {}

  [[nodiscard]] constexpr rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }
  [[nodiscard]] static constexpr Counter first() noexcept { return Counter(1); }
  [[nodiscard]] static constexpr Counter none() noexcept { return Counter(0); }

  // The next counter value, or nullopt when the counter space is exhausted.
  [[nodiscard]] constexpr std::optional<Counter> successor() const noexcept {
    if (value_ == std::numeric_limits<rep>::max()) {
      return std::nullopt;
    }
    return Counter(value_ + 1);
  }

  friend constexpr bool operator==(const Counter&, const Counter&) noexcept = default;
  friend constexpr auto operator<=>(const Counter&, const Counter&) noexcept = default;

 private:
  rep value_{0};
};

struct FeedTag;
struct BusTag;
struct UpsTag;
struct GeneratorTag;
struct PduTag;
struct CircuitTag;
struct LoadTag;
struct RedundancyGroupTag;
struct SourceTag;
struct MeasurementTag;

struct GenerationTag;
struct EpochTag;
struct RevisionTag;
struct SequenceTag;
struct MutationTag;
struct AttemptTag;
struct RecordTag;

using FeedId = StrongValue<FeedTag, std::string>;
using BusId = StrongValue<BusTag, std::string>;
using UpsId = StrongValue<UpsTag, std::string>;
using GeneratorId = StrongValue<GeneratorTag, std::string>;
using PduId = StrongValue<PduTag, std::string>;
using CircuitId = StrongValue<CircuitTag, std::string>;
using LoadId = StrongValue<LoadTag, std::string>;
using RedundancyGroupId = StrongValue<RedundancyGroupTag, std::string>;
using SourceId = StrongValue<SourceTag, std::string>;

// Monotonic counter carried by every piece of evidence. A generation changes
// whenever the producing authority publishes new evidence; evidence from an
// older generation is never silently merged with evidence from a newer one.
using Generation = Counter<GenerationTag>;
// Epoch of authoritative ownership. Attempts and mutations from an older epoch
// must be refused rather than applied.
using Epoch = Counter<EpochTag>;
// Revision of a derived artifact (snapshot, report, index).
using Revision = Counter<RevisionTag>;
// Ordering within a single source stream.
using Sequence = Counter<SequenceTag>;
// Durable identity of a mutation, used for retry/idempotency.
using MutationId = Counter<MutationTag>;
// Retry identity carried alongside a mutation.
using AttemptId = Counter<AttemptTag>;
// Durable identity of a single sample, unique within a source.
using MeasurementId = Counter<MeasurementTag>;
// Position of a record inside the durable evidence log.
using RecordIndex = Counter<RecordTag>;

// Component-wise ordering of a generation vector, used to detect whether one
// evidence set strictly dominates another.
enum class GenerationOrder : std::uint8_t {
  Equal = 0,
  Dominates = 1,
  Dominated = 2,
  Diverged = 3,
  Incomparable = 4,
};

[[nodiscard]] std::string_view to_string(GenerationOrder order) noexcept;

}  // namespace po
