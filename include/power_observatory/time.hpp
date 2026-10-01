// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

#include "power_observatory/quantity.hpp"
#include "power_observatory/result.hpp"

namespace po {

// A wall-clock instant in UTC, carried as nanoseconds since the Unix epoch.
// The representable range is 1678-09-21 .. 2262-04-11; values outside it are
// refused rather than wrapped. Wall-clock time may move backwards because of
// NTP or administrative action, so wall time is never used to measure age.
class Timestamp {
 public:
  using rep = std::int64_t;

  constexpr Timestamp() = default;

  [[nodiscard]] static constexpr Timestamp from_unix_nanos(rep nanos) noexcept { return Timestamp(nanos); }

  [[nodiscard]] static Result<Timestamp> from_unix_millis(rep millis) noexcept;
  [[nodiscard]] static Result<Timestamp> from_unix_micros(rep micros) noexcept;
  [[nodiscard]] static Result<Timestamp> from_unix_seconds(rep seconds_value) noexcept;

  // Strict RFC 3339 profile: YYYY-MM-DDTHH:MM:SS[.fraction][Z|+HH:MM|-HH:MM].
  // A missing offset means UTC. Leap seconds (SS == 60) are refused.
  [[nodiscard]] static Result<Timestamp> from_iso8601(std::string_view text);

  [[nodiscard]] constexpr rep unix_nanos() const noexcept { return nanos_; }
  [[nodiscard]] constexpr rep unix_micros() const noexcept { return floor_div(nanos_, 1000); }
  [[nodiscard]] constexpr rep unix_millis() const noexcept { return floor_div(nanos_, 1000000); }
  [[nodiscard]] constexpr rep unix_seconds() const noexcept { return floor_div(nanos_, 1000000000); }

  [[nodiscard]] constexpr bool is_epoch() const noexcept { return nanos_ == 0; }

  // Canonical rendering with exactly nine fractional digits, for example
  // "2026-01-15T08:30:00.000000000Z". Fixed width keeps the textual order
  // identical to the numeric order.
  [[nodiscard]] std::string to_iso8601() const;

  [[nodiscard]] static constexpr rep min_nanos() noexcept { return std::numeric_limits<rep>::min(); }
  [[nodiscard]] static constexpr rep max_nanos() noexcept { return std::numeric_limits<rep>::max(); }

  friend constexpr bool operator==(Timestamp, Timestamp) noexcept = default;
  friend constexpr auto operator<=>(Timestamp, Timestamp) noexcept = default;

 private:
  constexpr explicit Timestamp(rep nanos) noexcept : nanos_(nanos) {}

  [[nodiscard]] static constexpr rep floor_div(rep value, rep divisor) noexcept {
    const rep quotient = value / divisor;
    const rep remainder = value % divisor;
    if (remainder != 0 && (remainder < 0) != (divisor < 0)) {
      return quotient - 1;
    }
    return quotient;
  }

  rep nanos_{0};
};

// A monotonic instant. Its zero point is unspecified and differs between
// processes, so a monotonic instant is meaningful only for measuring elapsed
// time inside the process that observed it. It is never persisted as a
// durable value.
class MonotonicInstant {
 public:
  using rep = std::int64_t;

  constexpr MonotonicInstant() = default;
  [[nodiscard]] static constexpr MonotonicInstant from_nanos(rep nanos) noexcept {
    return MonotonicInstant(nanos);
  }
  [[nodiscard]] constexpr rep nanos() const noexcept { return nanos_; }

  friend constexpr bool operator==(MonotonicInstant, MonotonicInstant) noexcept = default;
  friend constexpr auto operator<=>(MonotonicInstant, MonotonicInstant) noexcept = default;

 private:
  constexpr explicit MonotonicInstant(rep nanos) noexcept : nanos_(nanos) {}
  rep nanos_{0};
};

[[nodiscard]] Result<Timestamp> add(Timestamp base, Duration delta) noexcept;
[[nodiscard]] Result<Duration> sub(Timestamp later, Timestamp earlier) noexcept;

[[nodiscard]] Result<MonotonicInstant> add(MonotonicInstant base, Duration delta) noexcept;
[[nodiscard]] Result<Duration> sub(MonotonicInstant later, MonotonicInstant earlier) noexcept;

// The clock abstraction exists so that every time-dependent decision in the
// runtime can be driven deterministically in tests and replayed exactly.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  virtual ~Clock() = default;

  [[nodiscard]] virtual Timestamp wall_now() const = 0;
  [[nodiscard]] virtual MonotonicInstant steady_now() const = 0;
};

class SystemClock final : public Clock {
 public:
  [[nodiscard]] static const SystemClock& instance() noexcept;
  [[nodiscard]] Timestamp wall_now() const override;
  [[nodiscard]] MonotonicInstant steady_now() const override;
};

// A clock that only moves when the test moves it. Thread-safe.
class ManualClock final : public Clock {
 public:
  ManualClock() noexcept;
  explicit ManualClock(Timestamp wall_start, MonotonicInstant steady_start = MonotonicInstant{}) noexcept;

  [[nodiscard]] Timestamp wall_now() const override;
  [[nodiscard]] MonotonicInstant steady_now() const override;

  [[nodiscard]] Status set_wall(Timestamp value) noexcept;
  [[nodiscard]] Status set_steady(MonotonicInstant value) noexcept;
  [[nodiscard]] Status advance_wall(Duration delta) noexcept;
  [[nodiscard]] Status advance_steady(Duration delta) noexcept;

 private:
  mutable std::mutex mutex_;
  Timestamp wall_{};
  MonotonicInstant steady_{};
};

namespace detail {

// Days since 1970-01-01 for a proleptic Gregorian date.
[[nodiscard]] constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  const std::int64_t adjusted_year = year - (month <= 2 ? 1 : 0);
  const std::int64_t era = (adjusted_year >= 0 ? adjusted_year : adjusted_year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(adjusted_year - era * 400);
  const unsigned day_of_year =
      (153u * (month > 2 ? month - 3u : month + 9u) + 2u) / 5u + day - 1u;
  const unsigned day_of_era = year_of_era * 365u + year_of_era / 4u - year_of_era / 100u + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

// Inverse of days_from_civil.
constexpr void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460u + day_of_era / 36524u - day_of_era / 146096u) / 365u;
  const std::int64_t computed_year = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year = day_of_era - (365u * year_of_era + year_of_era / 4u - year_of_era / 100u);
  const unsigned month_prime = (5u * day_of_year + 2u) / 153u;
  day = day_of_year - (153u * month_prime + 2u) / 5u + 1u;
  month = month_prime < 10u ? month_prime + 3u : month_prime - 9u;
  year = computed_year + (month <= 2u ? 1 : 0);
}

[[nodiscard]] constexpr bool is_leap_year(std::int64_t year) noexcept {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

[[nodiscard]] constexpr unsigned days_in_month(std::int64_t year, unsigned month) noexcept {
  switch (month) {
    case 1:
    case 3:
    case 5:
    case 7:
    case 8:
    case 10:
    case 12:
      return 31;
    case 4:
    case 6:
    case 9:
    case 11:
      return 30;
    case 2:
      return is_leap_year(year) ? 29u : 28u;
    default:
      return 0;
  }
}

}  // namespace detail

}  // namespace po
