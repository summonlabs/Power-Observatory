// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace po {

// FNV-1a over an explicitly little-endian byte encoding. The encoding is fixed
// so that a content fingerprint computed on one platform is identical on every
// other platform and is stable across releases of this library.
class Fnv1a64 {
 public:
  static constexpr std::uint64_t kOffsetBasis = 14695981039346656037ull;
  static constexpr std::uint64_t kPrime = 1099511628211ull;

  constexpr Fnv1a64() noexcept = default;

  constexpr void update_byte(std::uint8_t byte) noexcept {
    state_ ^= static_cast<std::uint64_t>(byte);
    state_ *= kPrime;
  }

  constexpr void update(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t index = 0; index < size; ++index) {
      update_byte(bytes[index]);
    }
  }

  constexpr void update(std::string_view text) noexcept { update(text.data(), text.size()); }

  constexpr void update_separator(char tag) noexcept {
    update_byte(static_cast<std::uint8_t>(tag));
    update_byte(0);
  }

  // Encodes the value little-endian so the fingerprint does not depend on the
  // host byte order. The single-byte case is split out so that no shift by the
  // full width of the type is ever formed.
  template <class T>
  constexpr void update_integral(T value) noexcept {
    static_assert(std::is_integral_v<T>, "update_integral requires an integral type");
    using U = std::make_unsigned_t<T>;
    U bits = static_cast<U>(value);
    if constexpr (sizeof(U) == 1) {
      update_byte(static_cast<std::uint8_t>(bits));
    } else {
      constexpr int kBytes = static_cast<int>(sizeof(U));
      for (int index = 0; index < kBytes; ++index) {
        update_byte(static_cast<std::uint8_t>(static_cast<U>(bits & static_cast<U>(0xFFu))));
        if (index + 1 < kBytes) {
          bits = static_cast<U>(bits >> 8);
        }
      }
    }
  }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return state_; }

 private:
  std::uint64_t state_{kOffsetBasis};
};

[[nodiscard]] constexpr std::uint64_t fnv1a64(std::string_view text) noexcept {
  Fnv1a64 hasher;
  hasher.update(text);
  return hasher.value();
}

// Convenience: hash a text field together with a tag so that concatenation
// ambiguity cannot produce the same fingerprint for different content.
[[nodiscard]] constexpr std::uint64_t combine_text(std::uint64_t seed, std::string_view text) noexcept {
  Fnv1a64 hasher;
  hasher.update_integral(seed);
  hasher.update_separator('|');
  hasher.update(text);
  return hasher.value();
}

}  // namespace po
