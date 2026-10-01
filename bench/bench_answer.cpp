// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Benchmark harness.
//
// Every number this program prints measures completed work: each iteration
// builds one complete answer to the core question. Nothing is extrapolated from
// a partial run and no result is reported for a workload that did not finish.
//
// The evidence basis is labelled honestly. The plant model is SYNTHETIC: it is
// produced by this repository's scenario generator, not by facility hardware.
// Everything measured on top of it -- persistence, recovery, snapshot assembly,
// answer assembly -- runs as REAL code against REAL files.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "power_observatory/divergence.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/json_reports.hpp"
#include "power_observatory/persistence.hpp"
#include "power_observatory/scenario.hpp"
#include "power_observatory/snapshot.hpp"
#include "power_observatory/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct BenchResult {
  std::string name;
  std::string basis;
  std::uint64_t completed{0};
  double nanoseconds_per_unit{0.0};
  std::string unit;
  std::string note;
};

[[nodiscard]] double elapsed_ns(Clock::time_point start, Clock::time_point finish) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count());
}

void report(const std::vector<BenchResult>& results) {
  std::cout << "Power Observatory benchmark harness\n";
  std::cout << po::build_info() << "\n\n";
  for (const BenchResult& result : results) {
    std::cout << result.name << "\n";
    std::cout << "  basis:      " << result.basis << "\n";
    std::cout << "  completed:  " << result.completed << " " << result.unit << "\n";
    std::printf("  cost:       %.1f ns per %s\n", result.nanoseconds_per_unit, result.unit.c_str());
    if (!result.note.empty()) {
      std::cout << "  note:       " << result.note << "\n";
    }
    std::cout << "\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t answer_iterations = 500;
  std::uint64_t steps = 60;
  std::uint64_t seed = 20260101;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto take = [&index, argc, argv]() -> std::uint64_t {
      if (index + 1 >= argc) {
        return 0;
      }
      return static_cast<std::uint64_t>(std::strtoull(argv[++index], nullptr, 10));
    };
    if (argument == "--answers") {
      answer_iterations = take();
    } else if (argument == "--steps") {
      steps = take();
    } else if (argument == "--seed") {
      seed = take();
    } else {
      std::cerr << "unrecognized argument " << argument << "\n";
      return 2;
    }
  }
  if (answer_iterations == 0 || steps == 0) {
    std::cerr << "iterations and steps must be positive\n";
    return 2;
  }

  std::vector<BenchResult> results;

  po::ScenarioOptions options;
  options.seed = seed;
  options.steps = static_cast<std::size_t>(steps);
  options.start_time = po::Timestamp::from_unix_millis(1767225600000LL).value();
  options.include_voltage = true;
  options.include_frequency = true;

  const po::Result<po::Scenario> scenario = po::build_standard_scenario(options);
  if (!scenario) {
    std::cerr << "scenario build failed: " << scenario.detail() << "\n";
    return 1;
  }

  const po::Timestamp stamp = po::Timestamp::from_unix_millis(1767225600000LL).value();
  const po::MonotonicInstant steady = po::MonotonicInstant::from_nanos(1000000000);
  std::size_t measurement_count = 0;
  for (const po::EvidenceBatch& batch : scenario.value().batches) {
    measurement_count += batch.measurements.size();
  }

  // --- snapshot construction ----------------------------------------------
  {
    const Clock::time_point started = Clock::now();
    for (std::uint64_t index = 0; index < steps; ++index) {
      po::Result<po::EvidenceSet> evidence =
          po::EvidenceSet::build(scenario.value().batches.back().measurements);
      if (!evidence) {
        std::cerr << "evidence build failed: " << evidence.detail() << "\n";
        return 1;
      }
      po::Result<po::Snapshot> built =
          po::Snapshot::build(po::Revision{1}, std::move(evidence).value(), scenario.value().topology,
                              po::default_policy(), stamp, steady);
      if (!built) {
        std::cerr << "snapshot build failed: " << built.detail() << "\n";
        return 1;
      }
    }
    const Clock::time_point finished = Clock::now();
    results.push_back(BenchResult{"snapshot construction", "SYNTHETIC evidence, REAL code path", steps,
                                  elapsed_ns(started, finished) / static_cast<double>(steps), "snapshot",
                                  "admits and canonically orders one full batch of evidence"});
  }

  std::vector<po::Measurement> admitted;
  for (const po::EvidenceBatch& batch : scenario.value().batches) {
    for (const po::Measurement& measurement : batch.measurements) {
      admitted.push_back(measurement);
    }
  }
  const po::Result<po::EvidenceSet> all_evidence = po::EvidenceSet::build(admitted);
  if (!all_evidence) {
    std::cerr << "evidence build failed: " << all_evidence.detail() << "\n";
    return 1;
  }
  const po::Result<po::Snapshot> snapshot =
      po::Snapshot::build(po::Revision{1}, all_evidence.value(), scenario.value().topology,
                          po::default_policy(),
                          po::Timestamp::from_unix_nanos(stamp.unix_nanos() + 1000000), steady);
  if (!snapshot) {
    std::cerr << "snapshot build failed: " << snapshot.detail() << "\n";
    return 1;
  }

  // --- answer assembly ----------------------------------------------------
  {
    std::uint64_t completed = 0;
    const Clock::time_point started = Clock::now();
    for (std::uint64_t index = 0; index < answer_iterations; ++index) {
      const po::AnswerReport answer = snapshot.value().answer();
      if (answer.explanation.reasons().empty()) {
        std::cerr << "an answer produced no reasons, which is a defect\n";
        return 1;
      }
      ++completed;
    }
    const Clock::time_point finished = Clock::now();
    results.push_back(BenchResult{"core question answer", "SYNTHETIC evidence, REAL code path", completed,
                                  elapsed_ns(started, finished) / static_cast<double>(completed), "answer",
                                  "flow, reserve, attribution, quality and failover, plus the explanation"});
  }

  // --- durable round trip -------------------------------------------------
  {
    const std::string directory =
        po::join_path(po::parent_directory(po::current_directory().value_or(".")), "po-bench-scratch");
    const po::Status made = po::create_directories(directory);
    if (!made) {
      std::cerr << "could not create the benchmark scratch directory\n";
      return 1;
    }
    const std::string log_path = po::join_path(directory, "bench.poev");
    static_cast<void>(po::remove_file(log_path));
    static_cast<void>(po::remove_file(log_path + ".lock"));

    po::StoreOpenOptions store_options;
    po::Result<po::EvidenceLog> log = po::EvidenceLog::open(log_path, po::Epoch{}, store_options);
    if (!log) {
      std::cerr << "evidence log open failed: " << log.detail() << "\n";
      return 1;
    }
    std::uint64_t committed = 0;
    const Clock::time_point started = Clock::now();
    for (const po::EvidenceBatch& batch : scenario.value().batches) {
      const po::Status appended = log.value().append(batch);
      if (!appended) {
        std::cerr << "append failed: " << appended.detail() << "\n";
        return 1;
      }
      ++committed;
    }
    const Clock::time_point finished = Clock::now();
    const std::uint64_t bytes = log.value().committed_length();
    const po::Status closed = log.value().close();
    if (!closed) {
      std::cerr << "close failed: " << closed.detail() << "\n";
      return 1;
    }
    results.push_back(BenchResult{
        "durable commit, including flush to stable storage", "REAL filesystem", committed,
        elapsed_ns(started, finished) / static_cast<double>(committed), "batch",
        std::to_string(bytes) + " bytes committed across " + std::to_string(committed) + " batch(es)"});

    const Clock::time_point reload_started = Clock::now();
    po::Result<po::EvidenceLog> reopened = po::EvidenceLog::open(log_path, po::Epoch{}, store_options);
    if (!reopened) {
      std::cerr << "reopen failed: " << reopened.detail() << "\n";
      return 1;
    }
    const Clock::time_point reload_finished = Clock::now();
    const std::size_t recovered = reopened.value().evidence().size();
    static_cast<void>(reopened.value().close());
    results.push_back(BenchResult{"cold recovery of the whole log", "REAL filesystem",
                                  static_cast<std::uint64_t>(recovered),
                                  elapsed_ns(reload_started, reload_finished), "recovered measurement",
                                  "reads, integrity checks, decodes and reindexes every record"});

    static_cast<void>(po::remove_file(log_path));
    static_cast<void>(po::remove_file(log_path + ".lock"));
    static_cast<void>(po::remove_directory_recursively(directory));
  }

  std::cout << "evidence basis: SYNTHETIC plant model (" << measurement_count << " measurements per " << steps
            << " step(s), " << scenario.value().topology.size() << " declared entities)\n";
  std::cout << "unsupported: no facility hardware, no BMS or DCIM feed, no multi-node cluster was exercised\n\n";
  report(results);
  return 0;
}
