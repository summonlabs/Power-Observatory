// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Runtime integration and the concurrency contract.
//
// Every check here targets one clause of the ownership rules stated in
// runtime.hpp. The gates are deterministic: no test waits on a timer to decide
// whether something is correct.
#include "test_harness.hpp"

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/runtime.hpp"
#include "power_observatory/scenario.hpp"

using namespace po;

namespace {

Result<std::unique_ptr<Observatory>> make_runtime(const std::string& suite, std::uint64_t seed = 1) {
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(seed, 1));
  if (!scenario) {
    return scenario.error();
  }
  RuntimeOptions options;
  options.log_path = pofix::join(suite, "evidence.poev");
  options.topology = scenario.value().topology;
  options.policy = default_policy();
  options.auto_start = true;
  return Observatory::create(options);
}

EvidenceBatch batch_from(const Scenario& scenario, std::size_t index, std::uint64_t mutation) {
  EvidenceBatch batch = scenario.batches[index % scenario.batches.size()];
  batch.mutation = MutationId(mutation);
  batch.recorded_at = Timestamp::from_unix_millis(1767225600000LL).value();
  stamp_delivery(batch, batch.recorded_at, MonotonicInstant::from_nanos(0));
  return batch;
}

}  // namespace

PO_TEST(runtime, start_publishes_the_recovered_state) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_start");
  PO_REQUIRE_OK(runtime);
  PO_CHECK_EQ(runtime.value()->state(), RuntimeState::Running);
  const SnapshotHandle snapshot = runtime.value()->current_snapshot();
  PO_REQUIRE(snapshot != nullptr);
  PO_CHECK_EQ(snapshot->metadata().revision.value(), std::uint64_t{1});
  PO_CHECK_OK(runtime.value()->stop());
  PO_CHECK_EQ(runtime.value()->state(), RuntimeState::Stopped);
  pofix::clear("runtime_start");
}

PO_TEST(runtime, every_submitted_batch_is_durable_or_refused_and_never_left_hanging) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_ingest", 5);
  PO_REQUIRE_OK(runtime);
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(5, 3));
  PO_REQUIRE_OK(scenario);

  std::vector<std::shared_future<IngestResult>> futures;
  for (std::uint64_t index = 0; index < 3; ++index) {
    Result<std::shared_future<IngestResult>> future =
        runtime.value()->submit(batch_from(scenario.value(), static_cast<std::size_t>(index), index + 1));
    PO_REQUIRE_OK(future);
    futures.push_back(std::move(future).value());
  }
  for (const std::shared_future<IngestResult>& future : futures) {
    const IngestResult result = future.get();
    PO_CHECK_EQ(result.disposition, IngestDisposition::Committed);
    PO_CHECK(result.record.value() < 3);
  }
  PO_CHECK_EQ(runtime.value()->current_revision().value(), std::uint64_t{4});
  const SnapshotHandle snapshot = runtime.value()->current_snapshot();
  PO_REQUIRE(snapshot != nullptr);
  PO_CHECK(snapshot->metadata().measurement_count > 0);
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_ingest");
}

PO_TEST(runtime, a_repeated_mutation_is_reported_as_an_idempotent_replay) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_replay", 6);
  PO_REQUIRE_OK(runtime);
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(6, 1));
  PO_REQUIRE_OK(scenario);

  Result<std::shared_future<IngestResult>> first = runtime.value()->submit(batch_from(scenario.value(), 0, 99));
  PO_REQUIRE_OK(first);
  PO_CHECK_EQ(first.value().get().disposition, IngestDisposition::Committed);

  Result<std::shared_future<IngestResult>> second = runtime.value()->submit(batch_from(scenario.value(), 0, 99));
  PO_REQUIRE_OK(second);
  const IngestResult result = second.value().get();
  PO_CHECK_EQ(result.disposition, IngestDisposition::IdempotentReplay);
  PO_CHECK_EQ(result.code, ReasonCode::IdempotentReplay);
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_replay");
}

PO_TEST(runtime, a_callback_runs_with_no_lock_held_and_may_call_back_in) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_callback", 7);
  PO_REQUIRE_OK(runtime);
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(7, 1));
  PO_REQUIRE_OK(scenario);

  // The callback blocks while the main thread exercises every public entry
  // point. If any lock were held across the callback, that thread would block
  // forever and this test would hang rather than pass.
  std::promise<void> entered;
  std::shared_future<void> entered_future = entered.get_future().share();
  std::promise<void> release;
  std::shared_future<void> release_future = release.get_future().share();

  std::atomic<int> calls{0};
  std::atomic<int> nested{0};
  const Result<Subscription> subscription = runtime.value()->subscribe([&](const Snapshot&) {
    const int call = calls.fetch_add(1);
    if (call != 0) {
      return;
    }
    entered.set_value();
    release_future.wait();
    Result<Subscription> inner = runtime.value()->subscribe([](const Snapshot&) {});
    if (inner.has_value()) {
      nested.fetch_add(1);
    }
    if (runtime.value()->current_snapshot() != nullptr) {
      nested.fetch_add(1);
    }
  });
  PO_REQUIRE_OK(subscription);

  Result<std::shared_future<IngestResult>> submitted = runtime.value()->submit(batch_from(scenario.value(), 0, 1));
  PO_REQUIRE_OK(submitted);
  entered_future.wait();

  PO_CHECK(runtime.value()->current_snapshot() != nullptr);
  PO_CHECK(runtime.value()->current_revision().value() >= 1);
  PO_CHECK_EQ(runtime.value()->state(), RuntimeState::Running);
  PO_CHECK_EQ(runtime.value()->pending_request_count(), std::size_t{0});
  const Result<Subscription> extra = runtime.value()->subscribe([](const Snapshot&) {});
  PO_REQUIRE_OK(extra);
  PO_CHECK_OK(runtime.value()->unsubscribe(extra.value()));

  release.set_value();
  PO_CHECK_EQ(submitted.value().get().disposition, IngestDisposition::Committed);
  PO_CHECK(calls.load() >= 1);
  PO_CHECK(nested.load() >= 1);
  PO_CHECK_EQ(runtime.value()->callback_failures(), std::size_t{0});
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_callback");
}

PO_TEST(runtime, a_throwing_callback_does_not_kill_the_worker) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_callback_throw", 8);
  PO_REQUIRE_OK(runtime);
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(8, 1));
  PO_REQUIRE_OK(scenario);

  PO_REQUIRE_OK(runtime.value()->subscribe([](const Snapshot&) { throw std::runtime_error("hostile subscriber"); }));

  Result<std::shared_future<IngestResult>> first = runtime.value()->submit(batch_from(scenario.value(), 0, 1));
  PO_REQUIRE_OK(first);
  PO_CHECK_EQ(first.value().get().disposition, IngestDisposition::Committed);
  Result<std::shared_future<IngestResult>> second = runtime.value()->submit(batch_from(scenario.value(), 0, 2));
  PO_REQUIRE_OK(second);
  PO_CHECK_EQ(second.value().get().disposition, IngestDisposition::Committed);
  PO_CHECK(runtime.value()->callback_failures() >= 2);
  PO_CHECK_EQ(runtime.value()->state(), RuntimeState::Running);
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_callback_throw");
}

PO_TEST(runtime, stopping_fulfils_every_future_including_the_abandoned_ones) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_shutdown", 9);
  PO_REQUIRE_OK(runtime);
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(9, 1));
  PO_REQUIRE_OK(scenario);

  constexpr std::size_t kRequests = 64;
  std::vector<std::shared_future<IngestResult>> futures;
  for (std::size_t index = 0; index < kRequests; ++index) {
    Result<std::shared_future<IngestResult>> future =
        runtime.value()->submit(batch_from(scenario.value(), 0, index + 1));
    PO_REQUIRE_OK(future);
    futures.push_back(std::move(future).value());
  }

  PO_CHECK_OK(runtime.value()->stop());
  std::size_t settled = 0;
  std::size_t committed = 0;
  for (const std::shared_future<IngestResult>& future : futures) {
    const IngestResult result = future.get();
    ++settled;
    if (result.disposition == IngestDisposition::Committed) {
      ++committed;
    }
  }
  PO_CHECK_EQ(settled, kRequests);
  PO_CHECK(committed >= 1);

  Result<std::shared_future<IngestResult>> late = runtime.value()->submit(batch_from(scenario.value(), 0, 1000));
  PO_CHECK(!late.has_value());
  PO_CHECK(late.code() == ReasonCode::NotStarted || late.code() == ReasonCode::ShuttingDown);
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_shutdown");
}

PO_TEST(runtime, the_queue_bound_is_enforced_rather_than_grown) {
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(10, 1));
  PO_REQUIRE_OK(scenario);

  RuntimeOptions options;
  options.log_path = pofix::join("runtime_bound", "evidence.poev");
  options.topology = scenario.value().topology;
  options.max_pending_requests = 2;
  options.auto_start = true;
  const Result<std::unique_ptr<Observatory>> runtime = Observatory::create(options);
  PO_REQUIRE_OK(runtime);

  bool saw_queue_full = false;
  for (std::size_t index = 0; index < 200; ++index) {
    Result<std::shared_future<IngestResult>> future =
        runtime.value()->submit(batch_from(scenario.value(), 0, index + 1));
    if (!future) {
      PO_CHECK_EQ(future.code(), ReasonCode::QueueFull);
      saw_queue_full = true;
      break;
    }
  }
  PO_CHECK(saw_queue_full);
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_bound");
}

PO_TEST(runtime, concurrent_readers_never_observe_a_partial_snapshot) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_readers", 11);
  PO_REQUIRE_OK(runtime);
  Result<Scenario> scenario = build_standard_scenario(pofix::scenario_options(11, 2));
  PO_REQUIRE_OK(scenario);

  std::atomic<bool> writing{true};
  std::atomic<std::uint64_t> reads{0};
  std::atomic<std::uint64_t> inconsistencies{0};

  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&runtime, &writing, &reads, &inconsistencies]() {
      while (writing.load()) {
        const SnapshotHandle snapshot = runtime.value()->current_snapshot();
        if (snapshot == nullptr) {
          inconsistencies.fetch_add(1);
          continue;
        }
        const SnapshotMetadata& metadata = snapshot->metadata();
        if (metadata.measurement_count != snapshot->evidence().size()) {
          inconsistencies.fetch_add(1);
        }
        if (metadata.evidence_hash != snapshot->evidence().content_hash()) {
          inconsistencies.fetch_add(1);
        }
        reads.fetch_add(1);
      }
    });
  }

  for (std::uint64_t index = 0; index < 16; ++index) {
    Result<std::shared_future<IngestResult>> future =
        runtime.value()->submit(batch_from(scenario.value(), static_cast<std::size_t>(index), index + 1));
    PO_REQUIRE_OK(future);
    static_cast<void>(future.value().get());
  }
  writing.store(false);
  for (std::thread& reader : readers) {
    reader.join();
  }

  PO_CHECK(reads.load() > 0);
  PO_CHECK_EQ(inconsistencies.load(), std::uint64_t{0});
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_readers");
}

PO_TEST(runtime, an_epoch_advance_is_durable_and_republished) {
  const Result<std::unique_ptr<Observatory>> runtime = make_runtime("runtime_epoch", 12);
  PO_REQUIRE_OK(runtime);
  PO_CHECK_EQ(runtime.value()->epoch().value(), std::uint64_t{0});
  PO_REQUIRE_OK(runtime.value()->advance_epoch(Epoch(4), "operator takeover"));
  PO_CHECK_EQ(runtime.value()->epoch().value(), std::uint64_t{4});
  PO_REQUIRE_ERR(runtime.value()->advance_epoch(Epoch(4), "same again"), ReasonCode::EpochRegression);
  PO_CHECK(runtime.value()->current_revision().value() >= 2);
  PO_CHECK_OK(runtime.value()->stop());
  pofix::clear("runtime_epoch");
}

PO_TEST_MAIN()
