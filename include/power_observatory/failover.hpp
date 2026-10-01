// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "power_observatory/context.hpp"

namespace po {

enum class FailoverReadiness : std::uint8_t {
  Unknown = 0,
  Ready = 1,
  Degraded = 2,
  NotReady = 3,
  Unsupported = 4,
};

[[nodiscard]] std::string_view to_string(FailoverReadiness readiness) noexcept;

// One named check. The evaluable flag distinguishes "the check ran and failed"
// from "this runtime has no evidence channel that could decide this", which are
// materially different answers and are never collapsed together.
struct FailoverGate {
  std::string gate;
  ReasonCode code{ReasonCode::None};
  bool mandatory{true};
  bool evaluable{true};
  bool passed{false};
  std::string detail;
  std::vector<EvidenceRef> evidence;

  friend bool operator==(const FailoverGate&, const FailoverGate&) noexcept = default;
  friend auto operator<=>(const FailoverGate&, const FailoverGate&) noexcept = default;
};

struct FailoverReport {
  FailoverReadiness readiness{FailoverReadiness::Unknown};
  RedundancyGroupId group;
  RedundancyTopology topology{RedundancyTopology::Unknown};
  std::uint32_t required_live{0};
  std::uint32_t currently_live{0};
  std::optional<Power> load_to_transfer;
  std::optional<Power> peer_headroom_after_single_failure;
  std::vector<FailoverGate> gates;
  Explanation explanation;
};

struct FailoverQuery {
  RedundancyGroupId group;
};

[[nodiscard]] Outcome<FailoverReport> compute_failover(const ObservationContext& context,
                                                       const FailoverQuery& query);

}  // namespace po
