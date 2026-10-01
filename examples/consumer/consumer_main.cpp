// Power Observatory downstream consumer example.
//
// This program is deliberately outside the Power Observatory build tree. It is
// built only through an installed package, using find_package, and it exercises
// the parts of the public surface a downstream consumer actually depends on:
// the synthetic scenario generator, snapshot assembly, the composed answer, and
// a real durable round trip through the evidence log.
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <power_observatory/divergence.hpp>
#include <power_observatory/file_io.hpp>
#include <power_observatory/json_reports.hpp>
#include <power_observatory/persistence.hpp>
#include <power_observatory/runtime.hpp>
#include <power_observatory/scenario.hpp>
#include <power_observatory/snapshot.hpp>
#include <power_observatory/version.hpp>

namespace {

int fail(const std::string& message) {
  std::cerr << "consumer: " << message << "\n";
  return 1;
}

}  // namespace

int main() {
  std::cout << "consumer of " << po::kProjectName << " " << po::kVersionString << "\n";
  std::cout << "library build: " << po::build_info() << "\n";

  po::ScenarioOptions options;
  options.seed = 4242;
  options.steps = 3;
  options.start_time = po::Timestamp::from_unix_millis(1767225600000LL).value();
  options.include_frequency = true;
  options.include_voltage = true;

  const po::Result<po::Scenario> scenario = po::build_standard_scenario(options);
  if (!scenario) {
    return fail("scenario build refused: " + scenario.detail());
  }
  for (po::EvidenceBatch& batch : const_cast<std::vector<po::EvidenceBatch>&>(scenario.value().batches)) {
    po::stamp_delivery(batch, options.start_time, po::MonotonicInstant::from_nanos(1));
  }

  const po::Timestamp as_of = po::Timestamp::from_unix_nanos(options.start_time.unix_nanos() + 1);
  const po::MonotonicInstant steady = po::MonotonicInstant::from_nanos(1);

  po::Result<po::EvidenceSet> evidence =
      po::EvidenceSet::build(scenario.value().batches.back().measurements);
  if (!evidence) {
    return fail("evidence build refused: " + evidence.detail());
  }
  const po::Result<po::Snapshot> snapshot =
      po::Snapshot::build(po::Revision{1}, std::move(evidence).value(), scenario.value().topology,
                          po::default_policy(), as_of, steady);
  if (!snapshot) {
    return fail("snapshot build refused: " + snapshot.detail());
  }

  const po::AnswerReport answer = snapshot.value().answer();
  std::cout << "answer state: " << po::to_string(answer.state) << "\n";
  std::cout << "measured entities: " << answer.measured_entities << "\n";
  std::cout << "reasons: " << answer.explanation.reasons().size() << "\n";
  if (answer.explanation.reasons().empty()) {
    return fail("an answer without reasons is not a trustworthy answer");
  }

  const std::string directory = po::join_path(po::current_directory().value_or("."), "consumer-scratch");
  if (!po::create_directories(directory)) {
    return fail("could not create the consumer scratch directory");
  }
  const std::string log_path = po::join_path(directory, "consumer.poev");
  static_cast<void>(po::remove_file(log_path));
  static_cast<void>(po::remove_file(log_path + ".lock"));

  po::StoreOpenOptions store_options;
  {
    po::Result<po::EvidenceLog> log = po::EvidenceLog::open(log_path, po::Epoch{}, store_options);
    if (!log) {
      return fail("evidence log open refused: " + log.detail());
    }
    for (const po::EvidenceBatch& batch : scenario.value().batches) {
      const po::Status appended = log.value().append(batch);
      if (!appended) {
        return fail("append refused: " + appended.detail());
      }
    }
    const po::Status closed = log.value().close();
    if (!closed) {
      return fail("close refused: " + closed.detail());
    }
  }
  {
    po::Result<po::EvidenceLog> reopened = po::EvidenceLog::open(log_path, po::Epoch{}, store_options);
    if (!reopened) {
      return fail("reopen refused: " + reopened.detail());
    }
    if (reopened.value().recovery().records_applied != scenario.value().batches.size()) {
      return fail("recovery did not replay every committed record");
    }
    for (const po::Measurement& measurement : reopened.value().evidence().measurements()) {
      if (!measurement.provenance.recovered()) {
        return fail("recovered evidence was not marked as recovered");
      }
    }
    std::cout << "recovered measurements: " << reopened.value().evidence().size() << "\n";
    const po::Status closed = reopened.value().close();
    if (!closed) {
      return fail("close refused: " + closed.detail());
    }
  }

  static_cast<void>(po::remove_file(log_path));
  static_cast<void>(po::remove_file(log_path + ".lock"));
  static_cast<void>(po::remove_directory_recursively(directory));

  std::cout << "consumer finished successfully\n";
  return 0;
}
