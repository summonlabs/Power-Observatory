// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "power_observatory/reason.hpp"
#include "power_observatory/result.hpp"

namespace po {

// Physical quantities are carried as signed 64-bit integers in a fixed
// sub-unit (milliwatts, millivolts, ...). Integer representation is deliberate:
// results must be bit-identical across platforms and optimisation levels, and
// every operation must be overflow-checked rather than silently wrapping.
using QuantityRep = std::int64_t;

namespace detail {

template <class T>
[[nodiscard]] constexpr bool add_overflow(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "add_overflow requires an integral type");
  using U = std::make_unsigned_t<T>;
  const U ua = static_cast<U>(a);
  const U ub = static_cast<U>(b);
  const U ur = static_cast<U>(static_cast<U>(ua + ub));
  out = static_cast<T>(ur);
  if constexpr (std::is_signed_v<T>) {
    constexpr int bits = std::numeric_limits<U>::digits;
    return ((~(ua ^ ub) & (ua ^ ur)) >> (bits - 1)) == 0;
  } else {
    return ur >= ua;
  }
}

template <class T>
[[nodiscard]] constexpr bool sub_overflow(T a, T b, T& out) noexcept {
  static_assert(std::is_integral_v<T>, "sub_overflow requires an integral type");
  using U = std::make_unsigned_t<T>;
  const U ua = static_cast<U>(a);
  const U ub = static_cast<U>(b);
  const U ur = static_cast<U>(static_cast<U>(ua - ub));
  out = static_cast<T>(ur);
  if constexpr (std::is_signed_v<T>) {
    constexpr int bits = std::numeric_limits<U>::digits;
    return (((ua ^ ub) & (ua ^ ur)) >> (bits - 1)) == 0;
  } else {
    return ua >= ub;
  }
}

// Full 64x64 -> 128 bit unsigned product, computed with 32-bit limbs so that
// no compiler intrinsic and no platform-specific wide type is required.
struct WideProduct {
  std::uint64_t hi{0};
  std::uint64_t lo{0};
};

[[nodiscard]] constexpr WideProduct umul_wide(std::uint64_t a, std::uint64_t b) noexcept {
  constexpr std::uint64_t kMask = 0xFFFFFFFFull;
  const std::uint64_t a_lo = a & kMask;
  const std::uint64_t a_hi = a >> 32;
  const std::uint64_t b_lo = b & kMask;
  const std::uint64_t b_hi = b >> 32;

  const std::uint64_t p0 = a_lo * b_lo;
  const std::uint64_t p1 = a_lo * b_hi;
  const std::uint64_t p2 = a_hi * b_lo;
  const std::uint64_t p3 = a_hi * b_hi;

  const std::uint64_t mid = (p0 >> 32) + (p1 & kMask) + (p2 & kMask);

  WideProduct result;
  result.lo = (p0 & kMask) | (mid << 32);
  // The true product is below 2^128, so this sum cannot exceed 64 bits and no
  // intermediate step can wrap.
  result.hi = p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
  return result;
}

[[nodiscard]] constexpr std::uint64_t magnitude(std::int64_t value) noexcept {
  return value < 0 ? (~static_cast<std::uint64_t>(value) + 1ull) : static_cast<std::uint64_t>(value);
}

}  // namespace detail

// Checked signed 64-bit arithmetic. Each function returns true and writes the
// exact result when the operation is representable, and returns false having
// written a saturated value when it is not. Callers must branch on the return
// value; the saturated value is present only so that diagnostics are useful.
[[nodiscard]] constexpr bool checked_add(QuantityRep a, QuantityRep b, QuantityRep& out) noexcept {
  return detail::add_overflow(a, b, out);
}

[[nodiscard]] constexpr bool checked_sub(QuantityRep a, QuantityRep b, QuantityRep& out) noexcept {
  return detail::sub_overflow(a, b, out);
}

[[nodiscard]] constexpr bool checked_neg(QuantityRep a, QuantityRep& out) noexcept {
  if (a == std::numeric_limits<QuantityRep>::min()) {
    out = std::numeric_limits<QuantityRep>::max();
    return false;
  }
  out = -a;
  return true;
}

[[nodiscard]] constexpr bool checked_mul(QuantityRep a, QuantityRep b, QuantityRep& out) noexcept {
  const bool negative = (a < 0) != (b < 0);
  const detail::WideProduct product = detail::umul_wide(detail::magnitude(a), detail::magnitude(b));
  constexpr std::uint64_t kMaxPositive = 0x7FFFFFFFFFFFFFFFull;
  constexpr std::uint64_t kMaxNegativeMagnitude = 0x8000000000000000ull;

  if (product.hi != 0) {
    out = negative ? std::numeric_limits<QuantityRep>::min() : std::numeric_limits<QuantityRep>::max();
    return false;
  }
  if (negative) {
    if (product.lo > kMaxNegativeMagnitude) {
      out = std::numeric_limits<QuantityRep>::min();
      return false;
    }
    out = static_cast<QuantityRep>(~product.lo + 1ull);
    return true;
  }
  if (product.lo > kMaxPositive) {
    out = std::numeric_limits<QuantityRep>::max();
    return false;
  }
  out = static_cast<QuantityRep>(product.lo);
  return true;
}

[[nodiscard]] constexpr bool checked_div(QuantityRep a, QuantityRep b, QuantityRep& out) noexcept {
  if (b == 0) {
    out = 0;
    return false;
  }
  if (a == std::numeric_limits<QuantityRep>::min() && b == -1) {
    out = std::numeric_limits<QuantityRep>::max();
    return false;
  }
  out = a / b;
  return true;
}

// Exact (a * b) / d with every intermediate step checked. The decomposition
// a = q*d + r keeps each partial product inside the representable range
// whenever the final result is representable, so the computed quotient is the
// truncated true quotient rather than an approximation.
[[nodiscard]] constexpr bool checked_mul_div(QuantityRep a, QuantityRep b, QuantityRep d,
                                             QuantityRep& out) noexcept {
  if (d == 0) {
    out = 0;
    return false;
  }
  const QuantityRep q = a / d;
  const QuantityRep r = a % d;
  QuantityRep high{};
  QuantityRep low{};
  QuantityRep low_quotient{};
  if (!checked_mul(q, b, high)) {
    out = high;
    return false;
  }
  if (!checked_mul(r, b, low)) {
    out = low;
    return false;
  }
  if (!checked_div(low, d, low_quotient)) {
    out = low_quotient;
    return false;
  }
  return checked_add(high, low_quotient, out);
}

namespace detail {

// Renders raw sub-units as a canonical decimal string. Trailing fractional
// zeros are removed, but the value is always exact: the sub-unit is a decimal
// fraction of the canonical unit.
[[nodiscard]] std::string format_scaled(QuantityRep raw, QuantityRep raw_per_canonical);

[[nodiscard]] int decimal_exponent(QuantityRep raw_per_canonical) noexcept;

}  // namespace detail

// A physical quantity tagged by its unit. The tag supplies the canonical unit
// name, the sub-unit name, and the exact ratio between them.
template <class UnitTag>
class Quantity {
 public:
  using unit_tag = UnitTag;
  using rep = QuantityRep;

  constexpr Quantity() = default;
  constexpr explicit Quantity(rep raw) noexcept : raw_(raw) {}

  [[nodiscard]] static constexpr Quantity from_raw(rep raw) noexcept { return Quantity(raw); }

  // From a whole number of canonical units, e.g. whole watts.
  [[nodiscard]] static Result<Quantity> from_canonical(rep whole) {
    rep scaled{};
    if (!checked_mul(whole, UnitTag::raw_per_canonical, scaled)) {
      return Error(ReasonCode::ArithmeticOverflow,
                   std::string(UnitTag::quantity_name) + ": canonical value is out of representable range");
    }
    return Quantity(scaled);
  }

  [[nodiscard]] constexpr rep raw() const noexcept { return raw_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return raw_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return raw_ < 0; }
  [[nodiscard]] constexpr bool is_positive() const noexcept { return raw_ > 0; }

  // Exact decimal spelling of the value in the canonical unit, without a unit
  // suffix, e.g. "1234.567".
  [[nodiscard]] std::string canonical_value_string() const {
    return detail::format_scaled(raw_, UnitTag::raw_per_canonical);
  }

  // Exact decimal spelling with the canonical unit suffix, e.g. "1234.567 W".
  [[nodiscard]] std::string to_string() const {
    return canonical_value_string() + " " + std::string(UnitTag::canonical_unit);
  }

  friend constexpr bool operator==(Quantity, Quantity) noexcept = default;
  friend constexpr auto operator<=>(Quantity, Quantity) noexcept = default;

 private:
  rep raw_{0};
};

struct PowerUnitTag {
  static constexpr std::string_view quantity_name = "power";
  static constexpr std::string_view raw_unit = "mW";
  static constexpr std::string_view canonical_unit = "W";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

struct ApparentPowerUnitTag {
  static constexpr std::string_view quantity_name = "apparent_power";
  static constexpr std::string_view raw_unit = "mVA";
  static constexpr std::string_view canonical_unit = "VA";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

struct ReactivePowerUnitTag {
  static constexpr std::string_view quantity_name = "reactive_power";
  static constexpr std::string_view raw_unit = "mvar";
  static constexpr std::string_view canonical_unit = "var";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

struct VoltageUnitTag {
  static constexpr std::string_view quantity_name = "voltage";
  static constexpr std::string_view raw_unit = "mV";
  static constexpr std::string_view canonical_unit = "V";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

struct CurrentUnitTag {
  static constexpr std::string_view quantity_name = "current";
  static constexpr std::string_view raw_unit = "mA";
  static constexpr std::string_view canonical_unit = "A";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

struct FrequencyUnitTag {
  static constexpr std::string_view quantity_name = "frequency";
  static constexpr std::string_view raw_unit = "mHz";
  static constexpr std::string_view canonical_unit = "Hz";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

struct EnergyUnitTag {
  static constexpr std::string_view quantity_name = "energy";
  static constexpr std::string_view raw_unit = "mJ";
  static constexpr std::string_view canonical_unit = "J";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

struct TemperatureUnitTag {
  static constexpr std::string_view quantity_name = "temperature";
  static constexpr std::string_view raw_unit = "mC";
  static constexpr std::string_view canonical_unit = "C";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

// Ratios are carried in parts per million so that power factor, load factor,
// state of charge, and derating factors share one exact representation.
struct RatioUnitTag {
  static constexpr std::string_view quantity_name = "ratio";
  static constexpr std::string_view raw_unit = "ppm";
  static constexpr std::string_view canonical_unit = "%";
  static constexpr QuantityRep raw_per_canonical = 10000;
};

// Durations are carried in nanoseconds.
struct DurationUnitTag {
  static constexpr std::string_view quantity_name = "duration";
  static constexpr std::string_view raw_unit = "ns";
  static constexpr std::string_view canonical_unit = "s";
  static constexpr QuantityRep raw_per_canonical = 1000000000;
};

struct ResistanceUnitTag {
  static constexpr std::string_view quantity_name = "resistance";
  static constexpr std::string_view raw_unit = "mohm";
  static constexpr std::string_view canonical_unit = "ohm";
  static constexpr QuantityRep raw_per_canonical = 1000;
};

using Power = Quantity<PowerUnitTag>;
using ApparentPower = Quantity<ApparentPowerUnitTag>;
using ReactivePower = Quantity<ReactivePowerUnitTag>;
using Voltage = Quantity<VoltageUnitTag>;
using Current = Quantity<CurrentUnitTag>;
using Frequency = Quantity<FrequencyUnitTag>;
using Energy = Quantity<EnergyUnitTag>;
using Temperature = Quantity<TemperatureUnitTag>;
using Ratio = Quantity<RatioUnitTag>;
using Duration = Quantity<DurationUnitTag>;
using Resistance = Quantity<ResistanceUnitTag>;

using Watt = Power;
using Milliwatt = Power;

// Ratio helpers. 100% == 1000000 ppm.
[[nodiscard]] constexpr Ratio ratio_from_ppm(QuantityRep ppm) noexcept { return Ratio::from_raw(ppm); }
[[nodiscard]] constexpr QuantityRep ratio_ppm(Ratio value) noexcept { return value.raw(); }
[[nodiscard]] constexpr bool ratio_is_valid_unit_interval(Ratio value) noexcept {
  return value.raw() >= 0 && value.raw() <= 1000000;
}

// Arithmetic. Every operation reports a reason code rather than wrapping.
template <class UnitTag>
[[nodiscard]] Result<Quantity<UnitTag>> add(Quantity<UnitTag> a, Quantity<UnitTag> b) {
  QuantityRep out{};
  if (!checked_add(a.raw(), b.raw(), out)) {
    return Error(ReasonCode::ArithmeticOverflow,
                 std::string(UnitTag::quantity_name) + ": addition overflowed 64-bit range");
  }
  return Quantity<UnitTag>::from_raw(out);
}

template <class UnitTag>
[[nodiscard]] Result<Quantity<UnitTag>> sub(Quantity<UnitTag> a, Quantity<UnitTag> b) {
  QuantityRep out{};
  if (!checked_sub(a.raw(), b.raw(), out)) {
    return Error(ReasonCode::ArithmeticOverflow,
                 std::string(UnitTag::quantity_name) + ": subtraction overflowed 64-bit range");
  }
  return Quantity<UnitTag>::from_raw(out);
}

template <class UnitTag>
[[nodiscard]] Result<Quantity<UnitTag>> negate(Quantity<UnitTag> a) {
  QuantityRep out{};
  if (!checked_neg(a.raw(), out)) {
    return Error(ReasonCode::ArithmeticOverflow,
                 std::string(UnitTag::quantity_name) + ": negation overflowed 64-bit range");
  }
  return Quantity<UnitTag>::from_raw(out);
}

template <class UnitTag>
[[nodiscard]] Result<Quantity<UnitTag>> multiply(Quantity<UnitTag> a, QuantityRep factor) {
  QuantityRep out{};
  if (!checked_mul(a.raw(), factor, out)) {
    return Error(ReasonCode::ArithmeticOverflow,
                 std::string(UnitTag::quantity_name) + ": multiplication overflowed 64-bit range");
  }
  return Quantity<UnitTag>::from_raw(out);
}

template <class UnitTag>
[[nodiscard]] Result<Quantity<UnitTag>> divide(Quantity<UnitTag> a, QuantityRep divisor) {
  if (divisor == 0) {
    return Error(ReasonCode::DivisionByZero, std::string(UnitTag::quantity_name) + ": division by zero");
  }
  QuantityRep out{};
  if (!checked_div(a.raw(), divisor, out)) {
    return Error(ReasonCode::ArithmeticOverflow,
                 std::string(UnitTag::quantity_name) + ": division overflowed 64-bit range");
  }
  return Quantity<UnitTag>::from_raw(out);
}

// Scale by an exact ratio expressed in parts per million. The full product is
// evaluated in a wider domain, so this cannot silently lose precision.
template <class UnitTag>
[[nodiscard]] Result<Quantity<UnitTag>> scale_by_ppm(Quantity<UnitTag> a, QuantityRep ppm) {
  QuantityRep out{};
  if (!checked_mul_div(a.raw(), ppm, 1000000, out)) {
    return Error(ReasonCode::ArithmeticOverflow,
                 std::string(UnitTag::quantity_name) + ": scaled value is out of representable range");
  }
  return Quantity<UnitTag>::from_raw(out);
}

// Ratio of a to b in ppm. Zero denominators are refused rather than producing
// a sentinel that could be mistaken for a real measurement.
template <class UnitTag>
[[nodiscard]] Result<Ratio> ratio_ppm_of(Quantity<UnitTag> a, Quantity<UnitTag> b) {
  if (b.is_zero()) {
    return Error(ReasonCode::DivisionByZero, std::string(UnitTag::quantity_name) + ": ratio against zero denominator");
  }
  QuantityRep out{};
  if (!checked_mul_div(a.raw(), 1000000, b.raw(), out)) {
    return Error(ReasonCode::ArithmeticOverflow, std::string(UnitTag::quantity_name) + ": ratio is out of range");
  }
  return Ratio::from_raw(out);
}

// A checked running total. Accumulation never wraps; the overflow flag makes a
// saturated total impossible to mistake for a real one.
template <class UnitTag>
class Accumulator {
 public:
  constexpr Accumulator() = default;

  constexpr void add(Quantity<UnitTag> value) noexcept {
    if (!checked_add(total_, value.raw(), total_)) {
      overflow_ = true;
    }
  }

  constexpr void subtract(Quantity<UnitTag> value) noexcept {
    if (!checked_sub(total_, value.raw(), total_)) {
      overflow_ = true;
    }
  }

  [[nodiscard]] constexpr bool overflowed() const noexcept { return overflow_; }
  [[nodiscard]] constexpr Quantity<UnitTag> saturated_total() const noexcept {
    return Quantity<UnitTag>::from_raw(total_);
  }

  [[nodiscard]] Result<Quantity<UnitTag>> total(std::string_view what) const {
    if (overflow_) {
      return Error(ReasonCode::ArithmeticOverflow,
                   std::string(what) + ": accumulated " + std::string(UnitTag::quantity_name) +
                       " exceeded 64-bit range");
    }
    return Quantity<UnitTag>::from_raw(total_);
  }

 private:
  QuantityRep total_{0};
  bool overflow_{false};
};

[[nodiscard]] constexpr Power milliwatts(QuantityRep value) noexcept { return Power::from_raw(value); }
[[nodiscard]] constexpr Voltage millivolts(QuantityRep value) noexcept { return Voltage::from_raw(value); }
[[nodiscard]] constexpr Current milliamperes(QuantityRep value) noexcept { return Current::from_raw(value); }
[[nodiscard]] constexpr Frequency millihertz(QuantityRep value) noexcept { return Frequency::from_raw(value); }
[[nodiscard]] constexpr Duration nanoseconds(QuantityRep value) noexcept { return Duration::from_raw(value); }
[[nodiscard]] constexpr Temperature millicelsius(QuantityRep value) noexcept { return Temperature::from_raw(value); }

[[nodiscard]] constexpr Duration milliseconds(QuantityRep value) noexcept {
  return Duration::from_raw(value * 1000000);
}
[[nodiscard]] constexpr Duration seconds(QuantityRep value) noexcept {
  return Duration::from_raw(value * 1000000000);
}

[[nodiscard]] Power min_power(Power a, Power b) noexcept;
[[nodiscard]] Power max_power(Power a, Power b) noexcept;

// Compact, human-readable duration rendering for reports, e.g. "1.5s", "250ms".
[[nodiscard]] std::string to_compact_string(Duration duration);

// Parses a decimal value in the canonical unit, e.g. "1234.567" for watts.
[[nodiscard]] Result<Power> parse_power(std::string_view text);
[[nodiscard]] Result<Voltage> parse_voltage(std::string_view text);
[[nodiscard]] Result<Current> parse_current(std::string_view text);
[[nodiscard]] Result<Frequency> parse_frequency(std::string_view text);
[[nodiscard]] Result<Ratio> parse_ratio_ppm(std::string_view text);
[[nodiscard]] Result<Duration> parse_duration(std::string_view text);
[[nodiscard]] Result<QuantityRep> parse_decimal_scaled(std::string_view text, QuantityRep raw_per_canonical,
                                                       std::string_view what);

}  // namespace po
