// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include "power_observatory/crc32c.hpp"

using namespace po;

PO_TEST(crc32c, known_check_vectors) {
  // Published CRC-32C check values. If these move, the on-disk format changes.
  PO_CHECK_EQ(Crc32c::compute(""), 0x00000000u);
  PO_CHECK_EQ(Crc32c::compute("a"), 0xC1D04330u);
  PO_CHECK_EQ(Crc32c::compute("123456789"), 0xE3069283u);
  PO_CHECK_EQ(Crc32c::compute("abc"), 0x364B3FB7u);
  PO_CHECK_EQ(Crc32c::compute("The quick brown fox jumps over the lazy dog"), 0x22620404u);
}

PO_TEST(crc32c, empty_buffer_via_pointer) {
  const std::string empty;
  PO_CHECK_EQ(Crc32c::compute(empty.data(), empty.size()), 0x00000000u);
}

PO_TEST(crc32c, incremental_matches_oneshot) {
  const std::string payload = "Power Observatory evidence record";
  Crc32c incremental;
  for (std::size_t split = 0; split <= payload.size(); ++split) {
    Crc32c partial;
    partial.update(payload.data(), split);
    partial.update(payload.data() + split, payload.size() - split);
    PO_CHECK_EQ(partial.value(), Crc32c::compute(payload));
  }
  incremental.update(payload);
  PO_CHECK_EQ(incremental.value(), Crc32c::compute(payload));
}

PO_TEST(crc32c, detects_every_single_bit_flip) {
  std::vector<std::uint8_t> bytes(64);
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::uint8_t>(index * 31 + 7);
  }
  const std::uint32_t baseline = Crc32c::compute(bytes.data(), bytes.size());
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    for (int bit = 0; bit < 8; ++bit) {
      std::vector<std::uint8_t> mutated = bytes;
      mutated[index] = static_cast<std::uint8_t>(mutated[index] ^ (1u << bit));
      PO_CHECK_NE(Crc32c::compute(mutated.data(), mutated.size()), baseline);
    }
  }
}

PO_TEST(crc32c, integral_encoding_is_little_endian) {
  Crc32c low;
  low.update_integral<std::uint16_t>(0x0102u);
  Crc32c bytes;
  const unsigned char raw[2] = {0x02, 0x01};
  bytes.update(raw, 2);
  PO_CHECK_EQ(low.value(), bytes.value());
}

PO_TEST_MAIN()
