// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "power_observatory/persistence.hpp"
#include "power_observatory/snapshot.hpp"
#include "power_observatory/time.hpp"

namespace po {

enum class RuntimeState : std::uint8_t {
  Created = 0,
  Running = 1,
  Stopping = 2,
  Stopped = 3,
  Failed = 4,
};

[[nodiscard]] std::string_view to_string(RuntimeState state) noexcept;

enum class IngestDisposition : std::uint8_t {
  Committed = 0,
  IdempotentReplay = 1,
  Rejected = 2,
  Cancelled = 3,
};

[[nodiscard]] std::string_view to_string(IngestDisposition disposition) noexcept;

struct IngestResult {
  IngestDisposition disposition{IngestDisposition::Rejected};
  ReasonCode code{ReasonCode::None};
  std::string detail;
  RecordIndex record{};
  Revision published_revision{};
};

struct RuntimeOptions {
  std::string log_path;
  Epoch epoch{};
  TopologyModel topology;
  ObservationPolicy policy{default_policy()};
  // Bound on queued-but-not-yet-processed ingest requests. Reaching it is
  // reported as QueueFull rather than growing without limit.
  std::size_t max_pending_requests{1024};
  StoreOpenOptions store;
  bool publish_on_ingest{true};
  bool auto_start{true};
  // Injected clock. Defaults to the system clock; tests supply a manual one.
  const Clock* clock{nullptr};
};

struct Subscription {
  std::uint64_t id{0};
  friend bool operator==(Subscription, Subscription) noexcept = default;
};

using SnapshotCallback = std::function<void(const Snapshot&)>;

// A single-writer, multi-reader observability runtime.
//
// Concurrency contract, stated once and enforced by construction:
//
//   * Exactly one worker thread mutates durable state and publishes snapshots.
//     Readers never mutate anything.
//   * A published snapshot is immutable and shared by pointer. A reader copies
//     the pointer under the publication mutex and releases it before touching
//     the snapshot, so no lock is ever held across a query.
//   * Subscriber callbacks are invoked on the worker thread with no lock held.
//     A callback may therefore call back into the runtime, including
//     subscribe() and current_snapshot(), without self-deadlocking.
//   * Lock order is queue_mutex_ -> publication_mutex_ -> subscribers_mutex_.
//     lifecycle_mutex_ is a leaf that is never held together with any other
//     mutex. No path acquires them in any other order.
//   * No mutex is held across thread creation, thread join, a callback, or a
//     wait on another thread.
//   * Read-modify-write of published state happens only on the worker thread,
//     so there is no read-to-write upgrade path.
//
// Shutdown contract: stop() refuses new work, wakes the worker, and joins it
// without holding any lock the worker needs. A request already in flight runs
// to completion; requests still queued are completed as Cancelled so that no
// caller is left waiting on a future that will never be fulfilled.
class Observatory {
 public:
  ~Observatory();
  Observatory(const Observatory&) = delete;
  Observatory& operator=(const Observatory&) = delete;
  Observatory(Observatory&&) = delete;
  Observatory& operator=(Observatory&&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<Observatory>> create(const RuntimeOptions& options);

  [[nodiscard]] Status start();
  [[nodiscard]] Status stop();

  // Enqueues a batch. The returned future is fulfilled once the batch has been
  // committed durably or refused; it is never left unfulfilled.
  [[nodiscard]] Result<std::shared_future<IngestResult>> submit(EvidenceBatch batch);

  [[nodiscard]] SnapshotHandle current_snapshot() const;
  [[nodiscard]] Revision current_revision() const;

  [[nodiscard]] Result<Subscription> subscribe(SnapshotCallback callback);
  [[nodiscard]] Status unsubscribe(Subscription subscription);

  // Recomputes and publishes a snapshot from the current durable state.
  [[nodiscard]] Status republish();

  [[nodiscard]] Status advance_epoch(Epoch epoch, std::string reason);

  [[nodiscard]] RuntimeState state() const noexcept;
  [[nodiscard]] Epoch epoch() const;
  [[nodiscard]] const RecoveryReport& recovery() const;
  [[nodiscard]] const std::string& log_path() const noexcept { return options_.log_path; }
  [[nodiscard]] std::size_t pending_request_count() const;
  [[nodiscard]] std::size_t subscriber_count() const;
  [[nodiscard]] std::size_t callback_failures() const noexcept { return callback_failures_.load(); }
  [[nodiscard]] IngestResult last_ingest_result() const;

 private:
  Observatory() = default;

  void worker_main();
  void process(EvidenceBatch batch, std::promise<IngestResult>& promise);
  [[nodiscard]] Status publish_locked_out();
  void notify_subscribers(const Snapshot& snapshot) noexcept;
  void drain_cancelled() noexcept;

  struct PendingRequest {
    EvidenceBatch batch;
    std::promise<IngestResult> promise;
  };

  struct Subscriber {
    std::uint64_t id{0};
    SnapshotCallback callback;
  };

  RuntimeOptions options_{};
  std::unique_ptr<EvidenceLog> log_;

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<PendingRequest> pending_;
  std::atomic<bool> stop_requested_{false};

  mutable std::mutex lifecycle_mutex_;
  RuntimeState state_{RuntimeState::Created};

  mutable std::mutex publication_mutex_;
  SnapshotHandle published_;
  Revision revision_{};

  mutable std::mutex subscribers_mutex_;
  std::vector<Subscriber> subscribers_;
  std::uint64_t next_subscription_id_{0};

  mutable std::mutex result_mutex_;
  IngestResult last_result_{};
  std::atomic<std::size_t> callback_failures_{0};

  std::thread worker_;
};

}  // namespace po
