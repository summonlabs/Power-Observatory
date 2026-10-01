// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/divergence.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace po {
namespace {

using Key = std::tuple<EntityRef, MeasurementKind, Phase>;

[[nodiscard]] std::map<Key, const Measurement*> index_of(const EvidenceSet& evidence) {
  std::map<Key, const Measurement*> result;
  for (const Measurement& measurement : evidence.measurements()) {
    const Key key = std::make_tuple(measurement.entity, measurement.kind(), measurement.phase);
    const Measurement* candidate = evidence.latest(measurement.entity, measurement.kind(), measurement.phase);
    if (candidate != nullptr) {
      result[key] = candidate;
    }
  }
  return result;
}

[[nodiscard]] DivergenceClass classify(const Measurement& left, const Measurement& right) {
  const QuantityRep left_raw = raw_value(left.value);
  const QuantityRep right_raw = raw_value(right.value);
  if (left_raw != right_raw) {
    if ((left_raw < 0) != (right_raw < 0)) {
      return DivergenceClass::SignFlip;
    }
    return DivergenceClass::ValueChanged;
  }
  if (left.provenance.generation != right.provenance.generation) {
    return left.provenance.generation < right.provenance.generation ? DivergenceClass::GenerationAdvanced
                                                                  : DivergenceClass::GenerationRegressed;
  }
  if (left.provenance.authority != right.provenance.authority) {
    return DivergenceClass::AuthorityChanged;
  }
  if (left.provenance.origin != right.provenance.origin) {
    return DivergenceClass::OriginChanged;
  }
  return DivergenceClass::None;
}

[[nodiscard]] GenerationOrder relation(const EvidenceSet& left, const EvidenceSet& right) {
  // The two source lists are materialised once. A by-value accessor called
  // twice inside one std::find would yield a pair of iterators belonging to two
  // different temporaries, which is undefined behaviour even when it happens to
  // appear to work.
  const std::vector<SourceId> left_sources = left.sources();
  const std::vector<SourceId> right_sources = right.sources();

  std::vector<SourceId> sources = left_sources;
  for (const SourceId& source : right_sources) {
    if (std::find(sources.begin(), sources.end(), source) == sources.end()) {
      sources.push_back(source);
    }
  }
  std::sort(sources.begin(), sources.end());

  bool left_ahead = false;
  bool right_ahead = false;
  for (const SourceId& source : sources) {
    const bool in_left = std::find(left_sources.begin(), left_sources.end(), source) != left_sources.end();
    const bool in_right = std::find(right_sources.begin(), right_sources.end(), source) != right_sources.end();
    if (in_left != in_right) {
      return GenerationOrder::Incomparable;
    }
    const Generation left_generation = left.generation_of(source);
    const Generation right_generation = right.generation_of(source);
    if (left_generation < right_generation) {
      right_ahead = true;
    } else if (left_generation > right_generation) {
      left_ahead = true;
    }
  }
  if (left_ahead && right_ahead) {
    return GenerationOrder::Diverged;
  }
  if (left_ahead) {
    return GenerationOrder::Dominates;
  }
  if (right_ahead) {
    return GenerationOrder::Dominated;
  }
  return GenerationOrder::Equal;
}

}  // namespace

std::string_view to_string(DivergenceClass classification) noexcept {
  switch (classification) {
    case DivergenceClass::None:
      return "none";
    case DivergenceClass::Appeared:
      return "appeared";
    case DivergenceClass::Disappeared:
      return "disappeared";
    case DivergenceClass::ValueChanged:
      return "value_changed";
    case DivergenceClass::SignFlip:
      return "sign_flip";
    case DivergenceClass::GenerationAdvanced:
      return "generation_advanced";
    case DivergenceClass::GenerationRegressed:
      return "generation_regressed";
    case DivergenceClass::AuthorityChanged:
      return "authority_changed";
    case DivergenceClass::OriginChanged:
      return "origin_changed";
  }
  return "none";
}

std::string_view to_string(DivergenceVerdict verdict) noexcept {
  switch (verdict) {
    case DivergenceVerdict::Identical:
      return "identical";
    case DivergenceVerdict::Advanced:
      return "advanced";
    case DivergenceVerdict::Regressed:
      return "regressed";
    case DivergenceVerdict::Diverged:
      return "diverged";
  }
  return "identical";
}

DivergenceReport compare(const EvidenceSet& left, const EvidenceSet& right) {
  DivergenceReport report;
  Explanation explanation;

  const std::map<Key, const Measurement*> left_index = index_of(left);
  const std::map<Key, const Measurement*> right_index = index_of(right);

  report.order = relation(left, right);

  auto record = [&](const DivergenceEntry& entry) {
    report.entries.push_back(entry);
    ++report.diverged_keys;
    switch (entry.classification) {
      case DivergenceClass::Appeared:
        ++report.appeared;
        break;
      case DivergenceClass::Disappeared:
        ++report.disappeared;
        break;
      case DivergenceClass::GenerationAdvanced:
      case DivergenceClass::GenerationRegressed:
        ++report.generation_changes;
        break;
      case DivergenceClass::ValueChanged:
      case DivergenceClass::SignFlip:
        ++report.value_changes;
        break;
      case DivergenceClass::AuthorityChanged:
      case DivergenceClass::OriginChanged:
      case DivergenceClass::None:
        break;
    }
  };

  for (const auto& entry : left_index) {
    ++report.compared_keys;
    const auto found = right_index.find(entry.first);
    if (found == right_index.end()) {
      DivergenceEntry divergence;
      divergence.entity = std::get<0>(entry.first);
      divergence.kind = std::get<1>(entry.first);
      divergence.phase = std::get<2>(entry.first);
      divergence.classification = DivergenceClass::Disappeared;
      divergence.left_raw = raw_value(entry.second->value);
      divergence.left_generation = entry.second->provenance.generation;
      divergence.detail = value_string(entry.second->value) + " was present on the left and is absent on the right";
      record(divergence);
      continue;
    }
    const DivergenceClass classification = classify(*entry.second, *found->second);
    if (classification == DivergenceClass::None) {
      continue;
    }
    DivergenceEntry divergence;
    divergence.entity = std::get<0>(entry.first);
    divergence.kind = std::get<1>(entry.first);
    divergence.phase = std::get<2>(entry.first);
    divergence.classification = classification;
    divergence.left_raw = raw_value(entry.second->value);
    divergence.right_raw = raw_value(found->second->value);
    divergence.left_generation = entry.second->provenance.generation;
    divergence.right_generation = found->second->provenance.generation;
    divergence.detail = std::string(po::to_string(classification)) + ": left " +
                        value_string(entry.second->value) + " (generation " +
                        std::to_string(entry.second->provenance.generation.value()) + "), right " +
                        value_string(found->second->value) + " (generation " +
                        std::to_string(found->second->provenance.generation.value()) + ")";
    record(divergence);
  }

  for (const auto& entry : right_index) {
    if (left_index.find(entry.first) != left_index.end()) {
      continue;
    }
    DivergenceEntry divergence;
    divergence.entity = std::get<0>(entry.first);
    divergence.kind = std::get<1>(entry.first);
    divergence.phase = std::get<2>(entry.first);
    divergence.classification = DivergenceClass::Appeared;
    divergence.right_raw = raw_value(entry.second->value);
    divergence.right_generation = entry.second->provenance.generation;
    divergence.detail = value_string(entry.second->value) + " is present on the right and was absent on the left";
    record(divergence);
  }

  std::sort(report.entries.begin(), report.entries.end());

  // The verdict describes the right evidence relative to the left. A left side
  // whose generations dominate is the newer one, so the right side has gone
  // backwards, and the other way round.
  if (report.entries.empty() && report.order == GenerationOrder::Equal) {
    report.verdict = DivergenceVerdict::Identical;
  } else if (report.order == GenerationOrder::Dominates) {
    report.verdict = DivergenceVerdict::Regressed;
  } else if (report.order == GenerationOrder::Dominated) {
    report.verdict = DivergenceVerdict::Advanced;
  } else {
    report.verdict = DivergenceVerdict::Diverged;
  }

  if (report.verdict == DivergenceVerdict::Identical) {
    explanation.add(ReasonCode::Ok, std::string("<divergence>"),
                    "the two evidence sets agree on every compared key and carry the same generations");
  } else {
    explanation.add(ReasonCode::SourceDisagreement, std::string("<divergence>"),
                    "verdict " + std::string(po::to_string(report.verdict)) + " over " +
                        std::to_string(report.compared_keys) + " compared key(s): " +
                        std::to_string(report.appeared) + " appeared, " + std::to_string(report.disappeared) +
                        " disappeared, " + std::to_string(report.value_changes) + " changed value, " +
                        std::to_string(report.generation_changes) + " changed generation");
    explanation.add(ReasonCode::Ok, std::string("<divergence>"),
                    "ledger right/left: " + std::string(po::to_string(report.order)) +
                        "; divergence is reported and never resolved by this runtime");
  }
  explanation.canonicalize();
  report.explanation = std::move(explanation);
  return report;
}

DivergenceReport compare(const Snapshot& left, const Snapshot& right) {
  DivergenceReport report = compare(left.evidence(), right.evidence());
  Explanation merged = report.explanation;
  merged.add(ReasonCode::Ok, std::string("<divergence>"),
             "compared snapshot revision " + std::to_string(left.metadata().revision.value()) + " against revision " +
                 std::to_string(right.metadata().revision.value()));
  if (left.metadata().topology_hash != right.metadata().topology_hash) {
    merged.add(ReasonCode::ConflictingTopology, std::string("<divergence>"),
               "the two snapshots were computed against different declared topologies");
  }
  if (left.metadata().policy_hash != right.metadata().policy_hash) {
    merged.add(ReasonCode::Ok, std::string("<divergence>"),
               "the two snapshots were computed against different observation policies, so a difference in the "
               "answers is not by itself a difference in the plant");
  }
  merged.canonicalize();
  report.explanation = std::move(merged);
  return report;
}

}  // namespace po
