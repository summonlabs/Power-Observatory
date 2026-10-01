// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/strong.hpp"

namespace po {

std::string_view to_string(GenerationOrder order) noexcept {
  switch (order) {
    case GenerationOrder::Equal:
      return "equal";
    case GenerationOrder::Dominates:
      return "dominates";
    case GenerationOrder::Dominated:
      return "dominated";
    case GenerationOrder::Diverged:
      return "diverged";
    case GenerationOrder::Incomparable:
      return "incomparable";
  }
  return "incomparable";
}

}  // namespace po
