// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/time.hpp"

#include <chrono>

namespace po {
namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000;
constexpr std::int64_t kSecondsPerDay = 86400;

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

void append_padded(std::string& out, std::int64_t value, std::size_t width) {
  std::string digits = std::to_string(value);
  if (digits.size() < width) {
    out.append(width - digits.size(), '0');
  }
  out.append(digits);
}

[[nodiscard]] Result<std::int64_t> parse_unsigned_field(std::string_view text, std::size_t offset,
                                                        std::size_t count) {
  std::int64_t value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char digit = text[offset + index];
    if (digit < '0' || digit > '9') {
      return Error(ReasonCode::ParseError, "timestamp: expected digit at offset " + std::to_string(offset + index));
    }
    value = value * 10 + static_cast<std::int64_t>(digit - '0');
  }
  return value;
}

}  // namespace

Result<Timestamp> Timestamp::from_unix_millis(rep millis) noexcept {
  rep nanos{};
  if (!checked_mul(millis, 1000000, nanos)) {
    return Error(ReasonCode::ValueOutOfRange, "timestamp: milliseconds value is outside the representable range");
  }
  return Timestamp::from_unix_nanos(nanos);
}

Result<Timestamp> Timestamp::from_unix_micros(rep micros) noexcept {
  rep nanos{};
  if (!checked_mul(micros, 1000, nanos)) {
    return Error(ReasonCode::ValueOutOfRange, "timestamp: microseconds value is outside the representable range");
  }
  return Timestamp::from_unix_nanos(nanos);
}

Result<Timestamp> Timestamp::from_unix_seconds(rep seconds_value) noexcept {
  rep nanos{};
  if (!checked_mul(seconds_value, kNanosPerSecond, nanos)) {
    return Error(ReasonCode::ValueOutOfRange, "timestamp: seconds value is outside the representable range");
  }
  return Timestamp::from_unix_nanos(nanos);
}

Result<Timestamp> Timestamp::from_iso8601(std::string_view raw_text) {
  const std::string_view text = trim(raw_text);
  const auto reject = [&](ReasonCode code, std::string detail) {
    return Result<Timestamp>(Error(code, "timestamp '" + std::string(raw_text) + "': " + std::move(detail)));
  };

  if (text.size() < 19) {
    return reject(ReasonCode::ParseError, "shorter than the minimum YYYY-MM-DDTHH:MM:SS form");
  }
  if (text[4] != '-' || text[7] != '-' || text[13] != ':' || text[16] != ':') {
    return reject(ReasonCode::ParseError, "missing date or time separators");
  }
  const char separator = text[10];
  if (separator != 'T' && separator != 't' && separator != ' ') {
    return reject(ReasonCode::ParseError, "missing date/time separator");
  }

  const Result<std::int64_t> year_value = parse_unsigned_field(text, 0, 4);
  if (!year_value) {
    return reject(year_value.code(), year_value.detail());
  }
  const Result<std::int64_t> month_value = parse_unsigned_field(text, 5, 2);
  if (!month_value) {
    return reject(month_value.code(), month_value.detail());
  }
  const Result<std::int64_t> day_value = parse_unsigned_field(text, 8, 2);
  if (!day_value) {
    return reject(day_value.code(), day_value.detail());
  }
  const Result<std::int64_t> hour_value = parse_unsigned_field(text, 11, 2);
  if (!hour_value) {
    return reject(hour_value.code(), hour_value.detail());
  }
  const Result<std::int64_t> minute_value = parse_unsigned_field(text, 14, 2);
  if (!minute_value) {
    return reject(minute_value.code(), minute_value.detail());
  }
  const Result<std::int64_t> second_value = parse_unsigned_field(text, 17, 2);
  if (!second_value) {
    return reject(second_value.code(), second_value.detail());
  }

  const std::int64_t year = year_value.value();
  const unsigned month = static_cast<unsigned>(month_value.value());
  const unsigned day = static_cast<unsigned>(day_value.value());
  const std::int64_t hour = hour_value.value();
  const std::int64_t minute = minute_value.value();
  const std::int64_t second = second_value.value();

  if (month < 1 || month > 12) {
    return reject(ReasonCode::ValueOutOfRange, "month is not in 1..12");
  }
  if (day < 1 || day > detail::days_in_month(year, month)) {
    return reject(ReasonCode::ValueOutOfRange, "day is not valid for the given month");
  }
  if (hour > 23) {
    return reject(ReasonCode::ValueOutOfRange, "hour is not in 0..23");
  }
  if (minute > 59) {
    return reject(ReasonCode::ValueOutOfRange, "minute is not in 0..59");
  }
  if (second > 59) {
    return reject(ReasonCode::ValueOutOfRange, "second is not in 0..59; leap seconds are not representable");
  }

  std::size_t cursor = 19;
  std::int64_t fraction_nanos = 0;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    std::size_t digits = 0;
    std::int64_t fraction = 0;
    while (cursor < text.size() && text[cursor] >= '0' && text[cursor] <= '9') {
      if (digits >= 9) {
        return reject(ReasonCode::ValueOutOfRange, "fractional seconds beyond nanosecond precision");
      }
      fraction = fraction * 10 + static_cast<std::int64_t>(text[cursor] - '0');
      ++digits;
      ++cursor;
    }
    if (digits == 0) {
      return reject(ReasonCode::ParseError, "fractional part is empty");
    }
    for (std::size_t pad = digits; pad < 9; ++pad) {
      fraction *= 10;
    }
    fraction_nanos = fraction;
  }

  std::int64_t offset_seconds = 0;
  if (cursor < text.size()) {
    const char sign = text[cursor];
    if (sign == 'Z' || sign == 'z') {
      ++cursor;
    } else if (sign == '+' || sign == '-') {
      ++cursor;
      if (cursor + 5 > text.size()) {
        return reject(ReasonCode::ParseError, "truncated numeric UTC offset");
      }
      const Result<std::int64_t> offset_hour = parse_unsigned_field(text, cursor, 2);
      if (!offset_hour) {
        return reject(offset_hour.code(), offset_hour.detail());
      }
      if (text[cursor + 2] != ':') {
        return reject(ReasonCode::ParseError, "numeric UTC offset is missing its colon");
      }
      const Result<std::int64_t> offset_minute = parse_unsigned_field(text, cursor + 3, 2);
      if (!offset_minute) {
        return reject(offset_minute.code(), offset_minute.detail());
      }
      if (offset_hour.value() > 23 || offset_minute.value() > 59) {
        return reject(ReasonCode::ValueOutOfRange, "numeric UTC offset is out of range");
      }
      offset_seconds = offset_hour.value() * 3600 + offset_minute.value() * 60;
      if (sign == '-') {
        offset_seconds = -offset_seconds;
      }
      cursor += 5;
    } else {
      return reject(ReasonCode::ParseError, "unexpected trailing characters");
    }
  }
  if (cursor != text.size()) {
    return reject(ReasonCode::ParseError, "unexpected trailing characters");
  }

  const std::int64_t days = detail::days_from_civil(year, month, day);
  std::int64_t total_seconds{};
  if (!checked_mul(days, kSecondsPerDay, total_seconds)) {
    return reject(ReasonCode::ValueOutOfRange, "date is outside the representable range");
  }
  std::int64_t day_seconds{};
  if (!checked_mul(hour, 3600, day_seconds) || !checked_add(day_seconds, minute * 60, day_seconds) ||
      !checked_add(day_seconds, second, day_seconds)) {
    return reject(ReasonCode::ValueOutOfRange, "time of day is outside the representable range");
  }
  if (!checked_add(total_seconds, day_seconds, total_seconds) ||
      !checked_sub(total_seconds, offset_seconds, total_seconds)) {
    return reject(ReasonCode::ValueOutOfRange, "instant is outside the representable range");
  }
  std::int64_t nanos{};
  if (!checked_mul(total_seconds, kNanosPerSecond, nanos) || !checked_add(nanos, fraction_nanos, nanos)) {
    return reject(ReasonCode::ValueOutOfRange, "instant is outside the representable nanosecond range");
  }
  return Timestamp::from_unix_nanos(nanos);
}

std::string Timestamp::to_iso8601() const {
  const rep seconds_total = floor_div(nanos_, kNanosPerSecond);
  const rep fraction = nanos_ - seconds_total * kNanosPerSecond;
  const rep days = floor_div(seconds_total, kSecondsPerDay);
  const rep second_of_day = seconds_total - days * kSecondsPerDay;

  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  detail::civil_from_days(days, year, month, day);

  std::string out;
  out.reserve(32);
  append_padded(out, year, 4);
  out.push_back('-');
  append_padded(out, static_cast<std::int64_t>(month), 2);
  out.push_back('-');
  append_padded(out, static_cast<std::int64_t>(day), 2);
  out.push_back('T');
  append_padded(out, second_of_day / 3600, 2);
  out.push_back(':');
  append_padded(out, (second_of_day % 3600) / 60, 2);
  out.push_back(':');
  append_padded(out, second_of_day % 60, 2);
  out.push_back('.');
  append_padded(out, fraction, 9);
  out.push_back('Z');
  return out;
}

Result<Timestamp> add(Timestamp base, Duration delta) noexcept {
  QuantityRep out{};
  if (!checked_add(base.unix_nanos(), delta.raw(), out)) {
    return Error(ReasonCode::ValueOutOfRange, "instant plus duration is outside the representable range");
  }
  return Timestamp::from_unix_nanos(out);
}

Result<Duration> sub(Timestamp later, Timestamp earlier) noexcept {
  QuantityRep out{};
  if (!checked_sub(later.unix_nanos(), earlier.unix_nanos(), out)) {
    return Error(ReasonCode::ValueOutOfRange, "timestamp difference is outside the representable range");
  }
  return Duration::from_raw(out);
}

Result<MonotonicInstant> add(MonotonicInstant base, Duration delta) noexcept {
  QuantityRep out{};
  if (!checked_add(base.nanos(), delta.raw(), out)) {
    return Error(ReasonCode::ValueOutOfRange, "monotonic instant plus duration is outside the representable range");
  }
  return MonotonicInstant::from_nanos(out);
}

Result<Duration> sub(MonotonicInstant later, MonotonicInstant earlier) noexcept {
  QuantityRep out{};
  if (!checked_sub(later.nanos(), earlier.nanos(), out)) {
    return Error(ReasonCode::ValueOutOfRange, "monotonic difference is outside the representable range");
  }
  return Duration::from_raw(out);
}

const SystemClock& SystemClock::instance() noexcept {
  static const SystemClock clock;
  return clock;
}

Timestamp SystemClock::wall_now() const {
  const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  return Timestamp::from_unix_nanos(static_cast<std::int64_t>(nanos));
}

MonotonicInstant SystemClock::steady_now() const {
  const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
  return MonotonicInstant::from_nanos(static_cast<std::int64_t>(nanos));
}

ManualClock::ManualClock() noexcept = default;

ManualClock::ManualClock(Timestamp wall_start, MonotonicInstant steady_start) noexcept
    : wall_(wall_start), steady_(steady_start) {}

Timestamp ManualClock::wall_now() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return wall_;
}

MonotonicInstant ManualClock::steady_now() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return steady_;
}

Status ManualClock::set_wall(Timestamp value) noexcept {
  const std::lock_guard<std::mutex> guard(mutex_);
  wall_ = value;
  return ok_status();
}

Status ManualClock::set_steady(MonotonicInstant value) noexcept {
  const std::lock_guard<std::mutex> guard(mutex_);
  steady_ = value;
  return ok_status();
}

Status ManualClock::advance_wall(Duration delta) noexcept {
  const std::lock_guard<std::mutex> guard(mutex_);
  QuantityRep next{};
  if (!checked_add(wall_.unix_nanos(), delta.raw(), next)) {
    return fail(ReasonCode::ValueOutOfRange, "manual clock wall advance would overflow");
  }
  wall_ = Timestamp::from_unix_nanos(next);
  return ok_status();
}

Status ManualClock::advance_steady(Duration delta) noexcept {
  const std::lock_guard<std::mutex> guard(mutex_);
  QuantityRep next{};
  if (!checked_add(steady_.nanos(), delta.raw(), next)) {
    return fail(ReasonCode::ValueOutOfRange, "manual clock steady advance would overflow");
  }
  steady_ = MonotonicInstant::from_nanos(next);
  return ok_status();
}

}  // namespace po
