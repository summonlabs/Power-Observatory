// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "power_observatory/evidence.hpp"
#include "power_observatory/result.hpp"
#include "power_observatory/strong.hpp"

namespace po {

// One source plus the generation of that source's evidence that a conclusion
// depends on. A conclusion whose dependencies cannot be stated is not a
// conclusion this runtime is willing to publish.
struct EvidenceDependency {
  SourceId source;
  Generation generation{};
  std::size_t measurement_count{0};

  friend bool operator==(const EvidenceDependency&, const EvidenceDependency&) noexcept = default;
  friend auto operator<=>(const EvidenceDependency&, const EvidenceDependency&) noexcept = default;
};

// A single, machine-readable statement about why an answer came out the way it
// did. Members are declared in canonical comparison order.
struct Reason {
  ReasonCode code{ReasonCode::None};
  Severity severity{Severity::Info};
  std::string subject;
  std::string detail;
  std::vector<EvidenceRef> evidence;

  friend bool operator==(const Reason&, const Reason&) noexcept = default;
  friend auto operator<=>(const Reason&, const Reason&) noexcept = default;
};

// An accumulator of reasons, evidence references, and dependencies.
//
// The accumulator is deliberately order-insensitive: reasons may be added in
// whatever order the computation discovers them, and canonicalize() produces
// the single ordering that every published surface relies on. Two runs that
// reach the same conclusion through different paths therefore publish
// byte-identical explanations.
class Explanation {
 public:
  Explanation() = default;

  void add(Reason reason);
  // Severity is taken from the reason code's table entry.
  void add(ReasonCode code, std::string subject, std::string detail);
  void add(ReasonCode code, Severity severity, std::string subject, std::string detail);
  void add(ReasonCode code, std::string subject, std::string detail, std::vector<EvidenceRef> evidence);

  void add_evidence(EvidenceRef reference);
  void add_evidence(const std::vector<EvidenceRef>& references);

  void add_dependency(const SourceId& source, Generation generation, std::size_t measurement_count);
  void add_dependencies(const std::vector<EvidenceDependency>& dependencies);

  void merge(const Explanation& other);

  [[nodiscard]] const std::vector<Reason>& reasons() const noexcept { return reasons_; }
  [[nodiscard]] const std::vector<EvidenceRef>& evidence() const noexcept { return evidence_; }
  [[nodiscard]] const std::vector<EvidenceDependency>& dependencies() const noexcept { return dependencies_; }

  [[nodiscard]] bool empty() const noexcept {
    return reasons_.empty() && evidence_.empty() && dependencies_.empty();
  }
  [[nodiscard]] bool has(ReasonCode code) const noexcept;
  [[nodiscard]] std::size_t count(ReasonCode code) const noexcept;
  [[nodiscard]] Severity worst_severity() const noexcept;
  [[nodiscard]] bool has_failure() const noexcept { return worst_severity() >= Severity::Error; }
  [[nodiscard]] Severity worst_severity_of_category(ReasonCategory category) const noexcept;

  // Sort and deduplicate every collection. Idempotent.
  void canonicalize();

  // Multi-line human-readable rendering of the canonical form.
  [[nodiscard]] std::string to_text() const;

  // Fingerprint of the canonical form. Computed over a canonicalised copy, so
  // two explanations with the same content always agree.
  [[nodiscard]] std::uint64_t content_hash() const;

 private:
  std::vector<Reason> reasons_;
  std::vector<EvidenceRef> evidence_;
  std::vector<EvidenceDependency> dependencies_;
};

// Result<T> with the explanation that produced it. A refusal still carries its
// explanation, so a caller can always show why a question had no answer.
template <class T>
class [[nodiscard]] Outcome {
 public:
  Outcome(T value, Explanation explanation)
      : value_(std::move(value)), explanation_(std::move(explanation)) {}

  Outcome(Error error, Explanation explanation)
      : error_(std::move(error)), explanation_(std::move(explanation)) {}

  [[nodiscard]] bool has_value() const noexcept { return value_.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T& value() const& { return *value_; }
  [[nodiscard]] T& value() & { return *value_; }
  [[nodiscard]] T&& value() && { return std::move(*value_); }

  [[nodiscard]] const Error& error() const& noexcept { return error_; }
  [[nodiscard]] ReasonCode code() const noexcept { return error_.code(); }
  [[nodiscard]] const std::string& detail() const noexcept { return error_.detail(); }

  [[nodiscard]] const Explanation& explanation() const noexcept { return explanation_; }
  [[nodiscard]] Explanation& explanation() noexcept { return explanation_; }

 private:
  std::optional<T> value_;
  Error error_;
  Explanation explanation_;
};

[[nodiscard]] inline std::string subject_for(const EntityRef& entity) { return entity.to_string(); }

[[nodiscard]] inline Explanation explanation_with(ReasonCode code, std::string subject, std::string detail) {
  Explanation explanation;
  explanation.add(code, std::move(subject), std::move(detail));
  return explanation;
}

}  // namespace po
