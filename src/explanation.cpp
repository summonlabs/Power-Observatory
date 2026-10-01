// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/explanation.hpp"

#include <algorithm>

#include "power_observatory/hashing.hpp"

namespace po {
namespace {

[[nodiscard]] bool same_statement(const Reason& left, const Reason& right) noexcept {
  return left.code == right.code && left.severity == right.severity && left.subject == right.subject &&
         left.detail == right.detail;
}

void deduplicate(std::vector<EvidenceRef>& references) {
  std::sort(references.begin(), references.end());
  references.erase(std::unique(references.begin(), references.end()), references.end());
}

}  // namespace

void Explanation::add(Reason reason) { reasons_.push_back(std::move(reason)); }

void Explanation::add(ReasonCode code, std::string subject, std::string detail) {
  add(code, po::severity(code), std::move(subject), std::move(detail));
}

void Explanation::add(ReasonCode code, Severity reason_severity, std::string subject, std::string detail) {
  Reason reason;
  reason.code = code;
  reason.severity = reason_severity;
  reason.subject = std::move(subject);
  reason.detail = std::move(detail);
  reasons_.push_back(std::move(reason));
}

void Explanation::add(ReasonCode code, std::string subject, std::string detail,
                      std::vector<EvidenceRef> evidence) {
  Reason reason;
  reason.code = code;
  reason.severity = po::severity(code);
  reason.subject = std::move(subject);
  reason.detail = std::move(detail);
  reason.evidence = std::move(evidence);
  reasons_.push_back(std::move(reason));
}

void Explanation::add_evidence(EvidenceRef reference) { evidence_.push_back(std::move(reference)); }

void Explanation::add_evidence(const std::vector<EvidenceRef>& references) {
  evidence_.insert(evidence_.end(), references.begin(), references.end());
}

void Explanation::add_dependency(const SourceId& source, Generation generation, std::size_t measurement_count) {
  EvidenceDependency dependency;
  dependency.source = source;
  dependency.generation = generation;
  dependency.measurement_count = measurement_count;
  dependencies_.push_back(std::move(dependency));
}

void Explanation::add_dependencies(const std::vector<EvidenceDependency>& dependencies) {
  dependencies_.insert(dependencies_.end(), dependencies.begin(), dependencies.end());
}

void Explanation::merge(const Explanation& other) {
  reasons_.insert(reasons_.end(), other.reasons_.begin(), other.reasons_.end());
  evidence_.insert(evidence_.end(), other.evidence_.begin(), other.evidence_.end());
  dependencies_.insert(dependencies_.end(), other.dependencies_.begin(), other.dependencies_.end());
}

bool Explanation::has(ReasonCode code) const noexcept {
  return std::any_of(reasons_.begin(), reasons_.end(),
                     [code](const Reason& reason) { return reason.code == code; });
}

std::size_t Explanation::count(ReasonCode code) const noexcept {
  return static_cast<std::size_t>(
      std::count_if(reasons_.begin(), reasons_.end(), [code](const Reason& reason) { return reason.code == code; }));
}

Severity Explanation::worst_severity() const noexcept {
  Severity worst = Severity::Info;
  for (const Reason& reason : reasons_) {
    if (reason.severity > worst) {
      worst = reason.severity;
    }
  }
  return worst;
}

Severity Explanation::worst_severity_of_category(ReasonCategory reason_category) const noexcept {
  Severity worst = Severity::Info;
  for (const Reason& reason : reasons_) {
    if (category(reason.code) == reason_category && reason.severity > worst) {
      worst = reason.severity;
    }
  }
  return worst;
}

void Explanation::canonicalize() {
  for (Reason& reason : reasons_) {
    deduplicate(reason.evidence);
  }
  std::sort(reasons_.begin(), reasons_.end());

  std::vector<Reason> merged;
  merged.reserve(reasons_.size());
  for (Reason& reason : reasons_) {
    if (!merged.empty() && same_statement(merged.back(), reason)) {
      merged.back().evidence.insert(merged.back().evidence.end(), reason.evidence.begin(), reason.evidence.end());
      deduplicate(merged.back().evidence);
      continue;
    }
    merged.push_back(std::move(reason));
  }
  reasons_ = std::move(merged);

  deduplicate(evidence_);

  std::sort(dependencies_.begin(), dependencies_.end());
  dependencies_.erase(std::unique(dependencies_.begin(), dependencies_.end()), dependencies_.end());
}

std::string Explanation::to_text() const {
  std::vector<Reason> reasons = reasons_;
  std::sort(reasons.begin(), reasons.end());

  std::string text;
  for (const Reason& reason : reasons) {
    text.append("  [");
    text.append(po::to_string(reason.severity));
    text.append("] ");
    text.append(po::to_string(reason.code));
    if (!reason.subject.empty()) {
      text.append(" ");
      text.append(reason.subject);
    }
    if (!reason.detail.empty()) {
      text.append(": ");
      text.append(reason.detail);
    }
    text.append("\n");
  }
  return text;
}

std::uint64_t Explanation::content_hash() const {
  std::vector<Reason> reasons = reasons_;
  for (Reason& reason : reasons) {
    deduplicate(reason.evidence);
  }
  std::sort(reasons.begin(), reasons.end());
  reasons.erase(std::unique(reasons.begin(), reasons.end()), reasons.end());

  std::vector<EvidenceRef> references = evidence_;
  deduplicate(references);

  std::vector<EvidenceDependency> dependencies = dependencies_;
  std::sort(dependencies.begin(), dependencies.end());
  dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());

  Fnv1a64 hasher;
  hasher.update_separator('R');
  hasher.update_integral(reasons.size());
  for (const Reason& reason : reasons) {
    hasher.update_integral(static_cast<std::uint16_t>(reason.code));
    hasher.update_integral(static_cast<std::uint8_t>(reason.severity));
    hasher.update_separator('s');
    hasher.update(reason.subject);
    hasher.update_separator('d');
    hasher.update(reason.detail);
    hasher.update_integral(reason.evidence.size());
    for (const EvidenceRef& reference : reason.evidence) {
      hasher.update(to_string(reference));
    }
  }
  hasher.update_separator('E');
  hasher.update_integral(references.size());
  for (const EvidenceRef& reference : references) {
    hasher.update(to_string(reference));
  }
  hasher.update_separator('D');
  hasher.update_integral(dependencies.size());
  for (const EvidenceDependency& dependency : dependencies) {
    hasher.update(dependency.source.view());
    hasher.update_integral(dependency.generation.value());
    hasher.update_integral(dependency.measurement_count);
  }
  return hasher.value();
}

}  // namespace po
