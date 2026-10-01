// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace po {
namespace crc32c_detail {

[[nodiscard]] constexpr std::array<std::uint32_t, 256> make_table() noexcept {
  std::array<std::uint32_t, 256> generated{};
  for (std::uint32_t index = 0; index < 256; ++index) {
    std::uint32_t remainder = index;
    for (int bit = 0; bit < 8; ++bit) {
      remainder = (remainder >> 1) ^ (0x82F63B78u & (0u - (remainder & 1u)));
    }
    generated[index] = remainder;
  }
  return generated;
}

// The table lives at namespace scope because a constexpr function may not hold
// a static local in C++20.
inline constexpr std::array<std::uint32_t, 256> kTable = make_table();

}  // namespace crc32c_detail

// CRC-32C (Castagnoli), reflected, initial value 0xFFFFFFFF, final xor
// 0xFFFFFFFF. Chosen over CRC-32 because it detects the burst errors that
// matter for a record log, and because the same polynomial is available in
// hardware on every platform this runtime targets.
class Crc32c {
 public:
  constexpr Crc32c() noexcept = default;

  constexpr void update(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t state = state_;
    for (std::size_t index = 0; index < size; ++index) {
      state = crc32c_detail::kTable[static_cast<std::uint8_t>(state ^ bytes[index])] ^ (state >> 8);
    }
    state_ = state;
  }

  constexpr void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  // Encodes the value little-endian so the checksum does not depend on the host
  // byte order. The single-byte case is split out so that no shift by the full
  // width of the type is ever formed.
  template <class T>
  constexpr void update_integral(T value) noexcept {
    static_assert(std::is_integral_v<T>, "update_integral requires an integral type");
    using U = std::make_unsigned_t<T>;
    U bits = static_cast<U>(value);
    if constexpr (sizeof(U) == 1) {
      update(&bits, 1);
    } else {
      unsigned char buffer[sizeof(U)] = {};
      constexpr int kBytes = static_cast<int>(sizeof(U));
      for (int index = 0; index < kBytes; ++index) {
        buffer[index] = static_cast<unsigned char>(static_cast<U>(bits & static_cast<U>(0xFFu)));
        if (index + 1 < kBytes) {
          bits = static_cast<U>(bits >> 8);
        }
      }
      update(buffer, sizeof(U));
    }
  }

  [[nodiscard]] constexpr std::uint32_t value() const noexcept { return state_ ^ 0xFFFFFFFFu; }

  [[nodiscard]] static constexpr std::uint32_t compute(const void* data, std::size_t size) noexcept {
    Crc32c crc;
    crc.update(data, size);
    return crc.value();
  }

  [[nodiscard]] static constexpr std::uint32_t compute(std::string_view text) noexcept {
    return compute(text.data(), text.size());
  }

 private:
  std::uint32_t state_{0xFFFFFFFFu};
};

}  // namespace po
