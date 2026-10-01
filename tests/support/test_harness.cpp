// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "test_harness.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <crtdbg.h>
#endif

namespace potest {
namespace {

struct FailureState {
  std::mutex mutex;
  bool failed{false};
  std::vector<std::string> messages;
};

FailureState& failure_state() {
  static FailureState state;
  return state;
}

void reset_failure_state() {
  FailureState& state = failure_state();
  const std::lock_guard<std::mutex> guard(state.mutex);
  state.failed = false;
  state.messages.clear();
}

struct CollectedFailures {
  bool failed{false};
  std::vector<std::string> messages;
};

CollectedFailures collect_failures() {
  FailureState& state = failure_state();
  const std::lock_guard<std::mutex> guard(state.mutex);
  CollectedFailures collected;
  collected.failed = state.failed;
  collected.messages = state.messages;
  return collected;
}

[[nodiscard]] std::string make_location(const char* file, int line) {
  return std::string(file) + ":" + std::to_string(line);
}

[[nodiscard]] bool matches_filters(const TestCase& test, const std::vector<std::string>& filters) {
  if (filters.empty()) {
    return true;
  }
  const std::string qualified = std::string(test.suite) + "." + test.name;
  for (const std::string& filter : filters) {
    if (qualified.find(filter) != std::string::npos) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> cases;
  return cases;
}

Registration::Registration(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
}

void report_failure(std::string message_with_location) {
  FailureState& state = failure_state();
  const std::lock_guard<std::mutex> guard(state.mutex);
  state.failed = true;
  state.messages.push_back(std::move(message_with_location));
}

void report_failure_at(const char* file, int line, std::string message) {
  report_failure(make_location(file, line) + ": " + std::move(message));
}

int run_all(int argc, char** argv) {
#if defined(_WIN32)
  // A failing assertion in a test must reach the log, never a modal dialog. A
  // dialog would block an unattended run, and this suite is expected to be run
  // unattended.
  ::_set_error_mode(_OUT_TO_STDERR);
  ::_set_abort_behavior(0, _CALL_REPORTFAULT);
#endif

  std::vector<std::string> filters;
  bool verbose = false;
  bool list_only = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--verbose" || argument == "-v") {
      verbose = true;
    } else if (argument == "--list") {
      list_only = true;
    } else if (argument == "--filter") {
      if (index + 1 >= argc) {
        std::cerr << "error: --filter requires a value\n";
        return 2;
      }
      filters.emplace_back(argv[++index]);
    } else if (argument.rfind("--filter=", 0) == 0) {
      filters.emplace_back(argument.substr(9));
    } else {
      std::cerr << "error: unrecognized argument '" << argument << "'\n";
      return 2;
    }
  }

  std::vector<TestCase>& cases = registry();
  if (list_only) {
    for (const TestCase& test : cases) {
      std::cout << test.suite << "." << test.name << "\n";
    }
    return 0;
  }

  std::size_t executed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failure_report;

  for (const TestCase& test : cases) {
    if (!matches_filters(test, filters)) {
      continue;
    }
    ++executed;
    reset_failure_state();
    const std::string qualified = std::string(test.suite) + "." + test.name;
    try {
      test.function();
    } catch (const AssertionFailure& failure) {
      report_failure(qualified + ": aborted by assertion: " + failure.message);
    } catch (const std::exception& error) {
      report_failure(qualified + ": unexpected exception: " + error.what());
    } catch (...) {
      report_failure(qualified + ": unexpected non-standard exception");
    }

    // Flush per case so that a process that dies mid-run still shows which case
    // it died in. A buffered log would lose exactly the evidence needed.
    std::cout.flush();
    std::cerr.flush();

    CollectedFailures collected = collect_failures();
    if (collected.failed) {
      ++failed;
      failure_report.push_back("FAIL " + qualified);
      for (const std::string& message : collected.messages) {
        failure_report.push_back("      " + message);
      }
    } else if (verbose) {
      std::cout << "PASS " << qualified << "\n";
    }
  }

  std::cout << "executed " << executed << " case(s): " << (executed - failed) << " passed, " << failed
            << " failed\n";
  for (const std::string& line : failure_report) {
    std::cout << line << "\n";
  }
  std::cout.flush();
  return failed == 0 ? 0 : 1;
}

}  // namespace potest