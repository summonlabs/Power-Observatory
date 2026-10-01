// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "power_observatory/evidence.hpp"
#include "power_observatory/persistence.hpp"
#include "power_observatory/result.hpp"
#include "power_observatory/topology.hpp"

namespace po {

// A deterministic synthetic plant.
//
// Everything this generator produces is SYNTHETIC. It exists so that the
// observation, attribution, and explanation machinery can be exercised against
// a plant whose true state is known exactly, including its faults. Nothing it
// produces is presented anywhere as real facility telemetry.
struct ScenarioOptions {
  std::uint64_t seed{1};
  std::size_t steps{1};
  std::string source{"synthetic-lab"};
  Generation generation{1};
  Epoch epoch{};
  Timestamp start_time{};
  Duration step{seconds(1)};
  // Deterministic fault injection, expressed in parts per million of the
  // generated measurements.
  std::uint32_t dropout_ppm{0};
  std::uint32_t contradiction_ppm{0};
  std::uint32_t stale_ppm{0};
  // Extra headroom removed from one feed, used to drive reserve and failover
  // gates into their failing branches.
  std::uint32_t derate_feed_ppm{0};
  bool include_frequency{true};
  bool include_voltage{true};
  bool include_power_factor{false};
};

struct Scenario {
  TopologyModel topology;
  std::vector<EvidenceBatch> batches;
  SourceId source;
  std::string description;

  // Entities in a stable order, for reporting and for tests.
  [[nodiscard]] std::vector<EntityRef> entities() const { return topology.entity_order(); }
};

[[nodiscard]] Result<Scenario> build_scenario(const ScenarioOptions& options);

// The canonical two-path plant used by the command line tool and by tests.
[[nodiscard]] Result<Scenario> build_standard_scenario(const ScenarioOptions& options);

// Deterministic pseudo-random source. SplitMix64 is used because it is short,
// fully specified, and identical on every platform.
class DeterministicRandom {
 public:
  explicit DeterministicRandom(std::uint64_t seed) noexcept : state_(seed) {}
  [[nodiscard]] std::uint64_t next() noexcept;
  [[nodiscard]] std::uint64_t bounded(std::uint64_t bound) noexcept;
  [[nodiscard]] std::int64_t between(std::int64_t low, std::int64_t high) noexcept;

 private:
  std::uint64_t state_;
};

}  // namespace po
