// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/quantity.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace po {
namespace {

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
    text.remove_suffix(1);
  }
  return text;
}

[[nodiscard]] constexpr bool is_ascii_digit(char character) noexcept {
  return character >= '0' && character <= '9';
}

[[nodiscard]] constexpr QuantityRep power_of_ten(int exponent) noexcept {
  QuantityRep value = 1;
  for (int index = 0; index < exponent; ++index) {
    value *= 10;
  }
  return value;
}

constexpr int kMaxFractionalDigits = 18;

struct UnitScale {
  std::string_view suffix;
  QuantityRep raw_per_unit;
};

// Parses "[+-]W[.F]" into raw sub-units of a unit whose scale is raw_per_unit.
// The value must be exactly representable; a value such as "1.5" milliwatts is
// refused rather than silently truncated.
[[nodiscard]] Result<QuantityRep> parse_exact(std::string_view raw_text, QuantityRep raw_per_unit,
                                              std::string_view what) {
  const std::string context(what);
  const std::string_view text = trim(raw_text);
  if (text.empty()) {
    return Error(ReasonCode::ParseError, context + ": empty numeric value");
  }
  if (raw_per_unit <= 0) {
    return Error(ReasonCode::InternalInvariant, context + ": unit scale must be positive");
  }

  std::size_t cursor = 0;
  bool negative = false;
  if (text[cursor] == '+' || text[cursor] == '-') {
    negative = text[cursor] == '-';
    ++cursor;
  }

  const std::size_t whole_start = cursor;
  while (cursor < text.size() && is_ascii_digit(text[cursor])) {
    ++cursor;
  }
  const std::size_t whole_end = cursor;

  std::size_t fraction_start = cursor;
  std::size_t fraction_end = cursor;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    fraction_start = cursor;
    while (cursor < text.size() && is_ascii_digit(text[cursor])) {
      ++cursor;
    }
    fraction_end = cursor;
  }

  if (cursor != text.size()) {
    return Error(ReasonCode::ParseError, context + ": unexpected character in numeric value");
  }
  if (whole_end == whole_start && fraction_end == fraction_start) {
    return Error(ReasonCode::ParseError, context + ": numeric value contains no digits");
  }

  const std::size_t fraction_digits = fraction_end - fraction_start;
  if (fraction_digits > kMaxFractionalDigits) {
    return Error(ReasonCode::ValueOutOfRange, context + ": more fractional digits than can be represented exactly");
  }

  QuantityRep whole = 0;
  for (std::size_t index = whole_start; index < whole_end; ++index) {
    const QuantityRep digit = static_cast<QuantityRep>(text[index] - '0');
    QuantityRep scaled{};
    if (!checked_mul(whole, 10, scaled) || !checked_add(scaled, digit, whole)) {
      return Error(ReasonCode::ValueOutOfRange, context + ": integer part is out of range");
    }
  }

  QuantityRep total{};
  if (!checked_mul(whole, raw_per_unit, total)) {
    return Error(ReasonCode::ValueOutOfRange, context + ": value is out of range");
  }

  if (fraction_digits > 0) {
    QuantityRep fraction = 0;
    for (std::size_t index = fraction_start; index < fraction_end; ++index) {
      const QuantityRep digit = static_cast<QuantityRep>(text[index] - '0');
      QuantityRep scaled{};
      if (!checked_mul(fraction, 10, scaled) || !checked_add(scaled, digit, fraction)) {
        return Error(ReasonCode::ValueOutOfRange, context + ": fractional part is out of range");
      }
    }
    const QuantityRep denominator = power_of_ten(static_cast<int>(fraction_digits));
    QuantityRep scaled{};
    if (!checked_mul(fraction, raw_per_unit, scaled)) {
      return Error(ReasonCode::ValueOutOfRange, context + ": value is out of range");
    }
    if (scaled % denominator != 0) {
      return Error(ReasonCode::ValueOutOfRange,
                   context + ": value is not exactly representable in the unit's base scale");
    }
    QuantityRep fraction_raw{};
    if (!checked_add(total, scaled / denominator, fraction_raw)) {
      return Error(ReasonCode::ValueOutOfRange, context + ": value is out of range");
    }
    total = fraction_raw;
  }

  if (negative) {
    QuantityRep negated{};
    if (!checked_neg(total, negated)) {
      return Error(ReasonCode::ValueOutOfRange, context + ": value is out of range");
    }
    total = negated;
  }
  return total;
}

// Applies an optional trailing unit suffix. The table is searched in order, so
// longer suffixes must be listed before shorter ones that they end with.
[[nodiscard]] Result<QuantityRep> parse_with_units(std::string_view raw_text, std::string_view what,
                                                   QuantityRep canonical_raw_per_unit, const UnitScale* table,
                                                   std::size_t table_size) {
  const std::string_view text = trim(raw_text);
  for (std::size_t index = 0; index < table_size; ++index) {
    if (text.size() > table[index].suffix.size() && text.ends_with(table[index].suffix)) {
      return parse_exact(text.substr(0, text.size() - table[index].suffix.size()), table[index].raw_per_unit, what);
    }
  }
  return parse_exact(text, canonical_raw_per_unit, what);
}

constexpr UnitScale kPowerUnits[] = {
    {"MW", 1000000000},
    {"kW", 1000000},
    {"mW", 1},
    {"W", 1000},
};

constexpr UnitScale kVoltageUnits[] = {
    {"kV", 1000000},
    {"mV", 1},
    {"V", 1000},
};

constexpr UnitScale kCurrentUnits[] = {
    {"kA", 1000000},
    {"mA", 1},
    {"A", 1000},
};

constexpr UnitScale kFrequencyUnits[] = {
    {"kHz", 1000000},
    {"mHz", 1},
    {"Hz", 1000},
};

constexpr UnitScale kRatioUnits[] = {
    {"ppm", 1},
    {"%", 10000},
};

constexpr UnitScale kDurationUnits[] = {
    {"ms", 1000000},
    {"us", 1000},
    {"\xC2\xB5s", 1000},
    {"ns", 1},
    {"h", 3600000000000},
    {"m", 60000000000},
    {"s", 1000000000},
};

}  // namespace

namespace detail {

int decimal_exponent(QuantityRep raw_per_canonical) noexcept {
  int exponent = 0;
  QuantityRep value = raw_per_canonical;
  while (value > 1) {
    value /= 10;
    ++exponent;
  }
  return exponent;
}

std::string format_scaled(QuantityRep raw, QuantityRep raw_per_canonical) {
  if (raw_per_canonical <= 0) {
    return "0";
  }
  const int decimals = decimal_exponent(raw_per_canonical);
  const bool negative = raw < 0;
  const std::uint64_t magnitude_value = magnitude(raw);
  const std::uint64_t scale = static_cast<std::uint64_t>(raw_per_canonical);
  const std::uint64_t whole = magnitude_value / scale;
  const std::uint64_t remainder = magnitude_value % scale;

  std::string result = std::to_string(whole);
  if (remainder != 0 && decimals > 0) {
    std::string fraction = std::to_string(remainder);
    fraction.insert(fraction.begin(), static_cast<std::size_t>(decimals) - fraction.size(), '0');
    while (!fraction.empty() && fraction.back() == '0') {
      fraction.pop_back();
    }
    if (!fraction.empty()) {
      result.push_back('.');
      result.append(fraction);
    }
  }
  if (negative) {
    result.insert(result.begin(), '-');
  }
  return result;
}

}  // namespace detail

Power min_power(Power a, Power b) noexcept { return a.raw() <= b.raw() ? a : b; }
Power max_power(Power a, Power b) noexcept { return a.raw() >= b.raw() ? a : b; }

std::string to_compact_string(Duration duration) {
  const std::uint64_t magnitude_value = detail::magnitude(duration.raw());
  if (magnitude_value >= 1000000000ull) {
    return detail::format_scaled(duration.raw(), 1000000000) + "s";
  }
  if (magnitude_value >= 1000000ull) {
    return detail::format_scaled(duration.raw(), 1000000) + "ms";
  }
  if (magnitude_value >= 1000ull) {
    return detail::format_scaled(duration.raw(), 1000) + "us";
  }
  return detail::format_scaled(duration.raw(), 1) + "ns";
}

Result<QuantityRep> parse_decimal_scaled(std::string_view text, QuantityRep raw_per_canonical,
                                         std::string_view what) {
  return parse_exact(text, raw_per_canonical, what);
}

Result<Power> parse_power(std::string_view text) {
  const Result<QuantityRep> parsed =
      parse_with_units(text, "power", 1000, kPowerUnits, std::size(kPowerUnits));
  if (!parsed) {
    return parsed.error();
  }
  return Power::from_raw(parsed.value());
}

Result<Voltage> parse_voltage(std::string_view text) {
  const Result<QuantityRep> parsed =
      parse_with_units(text, "voltage", 1000, kVoltageUnits, std::size(kVoltageUnits));
  if (!parsed) {
    return parsed.error();
  }
  return Voltage::from_raw(parsed.value());
}

Result<Current> parse_current(std::string_view text) {
  const Result<QuantityRep> parsed =
      parse_with_units(text, "current", 1000, kCurrentUnits, std::size(kCurrentUnits));
  if (!parsed) {
    return parsed.error();
  }
  return Current::from_raw(parsed.value());
}

Result<Frequency> parse_frequency(std::string_view text) {
  const Result<QuantityRep> parsed =
      parse_with_units(text, "frequency", 1000, kFrequencyUnits, std::size(kFrequencyUnits));
  if (!parsed) {
    return parsed.error();
  }
  return Frequency::from_raw(parsed.value());
}

Result<Ratio> parse_ratio_ppm(std::string_view text) {
  const Result<QuantityRep> parsed = parse_with_units(text, "ratio", 1, kRatioUnits, std::size(kRatioUnits));
  if (!parsed) {
    return parsed.error();
  }
  return Ratio::from_raw(parsed.value());
}

Result<Duration> parse_duration(std::string_view text) {
  const Result<QuantityRep> parsed =
      parse_with_units(text, "duration", 1000000000, kDurationUnits, std::size(kDurationUnits));
  if (!parsed) {
    return parsed.error();
  }
  return Duration::from_raw(parsed.value());
}

}  // namespace po
