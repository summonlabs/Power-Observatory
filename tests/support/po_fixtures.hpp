// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Shared fixtures for the test suites. Nothing here is part of the shipped
// library; it exists so that suites agree on how a scratch directory is created
// and how a synthetic plant is assembled.
#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#include "power_observatory/json_reports.hpp"
#include "power_observatory/scenario.hpp"
#include "power_observatory/snapshot.hpp"

namespace pofix {

[[nodiscard]] inline std::filesystem::path scratch_root() {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "power-observatory-tests";
  std::error_code error;
  std::filesystem::create_directories(root, error);
  return root;
}

// A per-suite scratch directory, emptied before use so that a previous run can
// never influence this one.
[[nodiscard]] inline std::string scratch(std::string_view suite) {
  const std::filesystem::path directory = scratch_root() / std::string(suite);
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  return directory.string();
}

// A file path inside a freshly emptied per-suite scratch directory.
[[nodiscard]] inline std::string join(std::string_view suite, std::string_view name) {
  return po::join_path(scratch(suite), name);
}

// A path inside a suite's scratch directory without emptying it first. Use this
// when a test writes several files in sequence and must not wipe what it has
// already written.
[[nodiscard]] inline std::string scratch_path(std::string_view suite, std::string_view name) {
  const std::filesystem::path directory = scratch_root() / std::string(suite);
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  return (directory / std::string(name)).string();
}

// A child process launched without a shell.
//
// The program and its arguments are handed to the operating system directly, so
// no quoting is involved and a path containing spaces needs no escaping. This
// matters because both the build tree and the scratch directory can contain
// them.
struct ChildProcess {
  std::intptr_t handle{-1};
  [[nodiscard]] bool valid() const noexcept { return handle != -1; }
};

[[nodiscard]] inline ChildProcess spawn_child(const std::string& program,
                                              const std::vector<std::string>& arguments) {
  ChildProcess child;
#ifdef _WIN32
  std::vector<const char*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(program.c_str());
  for (const std::string& argument : arguments) {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);
  child.handle = ::_spawnv(_P_NOWAIT, program.c_str(), argv.data());
#else
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 2);
  argv.push_back(const_cast<char*>(program.c_str()));
  for (const std::string& argument : arguments) {
    argv.push_back(const_cast<char*>(argument.c_str()));
  }
  argv.push_back(nullptr);
  ::pid_t pid = 0;
  if (::posix_spawn(&pid, program.c_str(), nullptr, nullptr, argv.data(), ::environ) == 0) {
    child.handle = static_cast<std::intptr_t>(pid);
  }
#endif
  return child;
}

// Waits for the child and returns its exit code. There is no deadline: the
// caller decides when the child ends, so a hang is a defect to diagnose rather
// than something to time out.
[[nodiscard]] inline int wait_child(const ChildProcess& child) {
#ifdef _WIN32
  if (child.handle == -1) {
    return -1;
  }
  int status = 0;
  if (::_cwait(&status, child.handle, 0) == -1) {
    return -1;
  }
  return status;
#else
  if (child.handle <= 0) {
    return -1;
  }
  int status = 0;
  if (::waitpid(static_cast<::pid_t>(child.handle), &status, 0) < 0) {
    return -1;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

[[nodiscard]] inline int run_child(const std::string& program, const std::vector<std::string>& arguments) {
  const ChildProcess child = spawn_child(program, arguments);
  if (!child.valid()) {
    return -1;
  }
  return wait_child(child);
}

[[nodiscard]] inline void clear(std::string_view suite) {
  std::error_code error;
  std::filesystem::remove_all(scratch_root() / std::string(suite), error);
}

[[nodiscard]] inline po::ScenarioOptions scenario_options(std::uint64_t seed = 7, std::size_t steps = 1) {
  po::ScenarioOptions options;
  options.seed = seed;
  options.steps = steps;
  options.start_time = po::Timestamp::from_unix_millis(1767225600000LL).value();  // 2026-01-01T00:00:00Z
  return options;
}

// Evidence that has been delivered by this process, so it is eligible for the
// fresh classification. Tests that specifically exercise recovered evidence
// rebuild it through the durable store instead.
[[nodiscard]] inline void stamp_live(po::Scenario& scenario, po::Timestamp now, po::MonotonicInstant steady) {
  for (po::EvidenceBatch& batch : scenario.batches) {
    po::stamp_delivery(batch, now, steady);
  }
}

[[nodiscard]] inline po::Result<po::EvidenceSet> evidence_from(const po::Scenario& scenario) {
  std::vector<po::Measurement> measurements;
  for (const po::EvidenceBatch& batch : scenario.batches) {
    for (const po::Measurement& measurement : batch.measurements) {
      measurements.push_back(measurement);
    }
  }
  return po::EvidenceSet::build(std::move(measurements));
}

[[nodiscard]] inline po::Result<po::Snapshot> snapshot_from(const po::Scenario& scenario, po::Timestamp as_of,
                                                           po::MonotonicInstant steady,
                                                           po::ObservationPolicy policy = po::default_policy()) {
  po::Result<po::EvidenceSet> evidence = evidence_from(scenario);
  if (!evidence) {
    return evidence.error();
  }
  return po::Snapshot::build(po::Revision{1}, std::move(evidence).value(), scenario.topology, policy, as_of, steady);
}

[[nodiscard]] inline po::ObservationPolicy tolerant_policy() {
  po::ObservationPolicy policy;
  policy.freshness.default_budget.fresh_within = po::seconds(3600);
  policy.freshness.default_budget.aging_within = po::seconds(7200);
  policy.freshness.default_budget.stale_within = po::seconds(86400);
  return policy;
}

}  // namespace pofix
