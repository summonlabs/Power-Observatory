// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/runtime.hpp"

#include <algorithm>
#include <exception>
#include <string>
#include <utility>

namespace po {

std::string_view to_string(RuntimeState state) noexcept {
  switch (state) {
    case RuntimeState::Created:
      return "created";
    case RuntimeState::Running:
      return "running";
    case RuntimeState::Stopping:
      return "stopping";
    case RuntimeState::Stopped:
      return "stopped";
    case RuntimeState::Failed:
      return "failed";
  }
  return "created";
}

std::string_view to_string(IngestDisposition disposition) noexcept {
  switch (disposition) {
    case IngestDisposition::Committed:
      return "committed";
    case IngestDisposition::IdempotentReplay:
      return "idempotent_replay";
    case IngestDisposition::Rejected:
      return "rejected";
    case IngestDisposition::Cancelled:
      return "cancelled";
  }
  return "rejected";
}

Observatory::~Observatory() {
  // Any failure here is already reflected in the lifecycle state; a destructor
  // must not throw.
  static_cast<void>(stop());
}

Result<std::unique_ptr<Observatory>> Observatory::create(const RuntimeOptions& options) {
  if (options.log_path.empty()) {
    return Error(ReasonCode::InvalidArgument, "runtime requires an evidence log path");
  }
  if (options.max_pending_requests == 0) {
    return Error(ReasonCode::InvalidArgument, "runtime queue bound must be at least one");
  }
  const Status policy_valid = options.policy.validate();
  if (!policy_valid) {
    return policy_valid.error();
  }

  auto runtime = std::unique_ptr<Observatory>(new Observatory());
  runtime->options_ = options;

  Result<EvidenceLog> log = EvidenceLog::open(options.log_path, options.epoch, options.store);
  if (!log) {
    return log.error();
  }
  runtime->log_ = std::make_unique<EvidenceLog>(std::move(log).value());

  if (options.auto_start) {
    const Status started = runtime->start();
    if (!started) {
      return started.error();
    }
  }
  return runtime;
}

Status Observatory::start() {
  {
    const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    if (state_ == RuntimeState::Running) {
      return fail(ReasonCode::AlreadyStarted, "runtime is already running");
    }
    if (state_ == RuntimeState::Stopping) {
      return fail(ReasonCode::ShuttingDown, "runtime is shutting down");
    }
    state_ = RuntimeState::Running;
  }
  stop_requested_.store(false);
  worker_ = std::thread([this] { worker_main(); });

  // The first snapshot is published before start() returns so that a caller can
  // immediately observe the state that was recovered from durable storage.
  const Status published = republish();
  if (!published) {
    static_cast<void>(stop());
    return published;
  }
  return ok_status();
}

Status Observatory::stop() {
  {
    const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    if (state_ == RuntimeState::Stopped) {
      return ok_status();
    }
    if (state_ == RuntimeState::Created) {
      state_ = RuntimeState::Stopped;
      return ok_status();
    }
    state_ = RuntimeState::Stopping;
  }

  stop_requested_.store(true);
  queue_cv_.notify_all();

  // The join happens with no lock held: the worker needs queue_mutex_ and
  // lifecycle_mutex_ to finish, so holding either here would deadlock.
  if (worker_.joinable()) {
    worker_.join();
  }

  {
    const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    state_ = RuntimeState::Stopped;
  }

  if (log_ != nullptr) {
    return log_->close();
  }
  return ok_status();
}

RuntimeState Observatory::state() const noexcept {
  const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
  return state_;
}

void Observatory::drain_cancelled() noexcept {
  std::deque<PendingRequest> abandoned;
  {
    const std::lock_guard<std::mutex> guard(queue_mutex_);
    abandoned.swap(pending_);
  }
  for (PendingRequest& request : abandoned) {
    IngestResult result;
    result.disposition = IngestDisposition::Cancelled;
    result.code = ReasonCode::Cancelled;
    result.detail = "the runtime stopped before this request was processed";
    try {
      request.promise.set_value(result);
    } catch (const std::future_error&) {
      // The caller already abandoned the future; nothing to report.
    }
  }
}

void Observatory::worker_main() {
  for (;;) {
    PendingRequest request;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return !pending_.empty() || stop_requested_.load(); });
      if (pending_.empty()) {
        break;
      }
      request = std::move(pending_.front());
      pending_.pop_front();
    }
    // The lock is released before any work happens, so the worker never holds
    // queue_mutex_ while touching durable state, publishing, or calling out.
    process(std::move(request.batch), request.promise);
  }
  drain_cancelled();
}

void Observatory::process(EvidenceBatch batch, std::promise<IngestResult>& promise) {
  IngestResult result;
  Status appended = fail(ReasonCode::InternalInvariant, "ingest did not run");

  if (log_ == nullptr) {
    result.disposition = IngestDisposition::Rejected;
    result.code = ReasonCode::InternalInvariant;
    result.detail = "the runtime has no open evidence log";
  } else {
    const RecordIndex index = log_->next_record_index();
    appended = log_->append(batch);
    if (appended) {
      result.disposition = IngestDisposition::Committed;
      result.code = ReasonCode::Ok;
      result.record = index;
      result.detail = "committed as durable record " + std::to_string(index.value());
    } else if (appended.code() == ReasonCode::IdempotentReplay) {
      result.disposition = IngestDisposition::IdempotentReplay;
      result.code = appended.code();
      result.detail = appended.detail();
    } else {
      result.disposition = IngestDisposition::Rejected;
      result.code = appended.code();
      result.detail = appended.detail();
    }
  }

  if (appended && options_.publish_on_ingest) {
    const Status published = republish();
    if (!published) {
      result.detail.append("; the batch is durable but the follow-up snapshot could not be published: ");
      result.detail.append(published.detail());
    }
  }

  {
    const std::lock_guard<std::mutex> guard(result_mutex_);
    last_result_ = result;
  }

  try {
    promise.set_value(result);
  } catch (const std::future_error&) {
    // The caller abandoned the future. The work is already durable.
  }
}

Result<std::shared_future<IngestResult>> Observatory::submit(EvidenceBatch batch) {
  {
    const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    if (state_ != RuntimeState::Running) {
      return Error(state_ == RuntimeState::Stopping ? ReasonCode::ShuttingDown : ReasonCode::NotStarted,
                   "runtime is " + std::string(po::to_string(state_)) + " and cannot accept evidence");
    }
  }

  PendingRequest request;
  request.batch = std::move(batch);
  std::shared_future<IngestResult> future = request.promise.get_future().share();

  {
    const std::lock_guard<std::mutex> guard(queue_mutex_);
    if (pending_.size() >= options_.max_pending_requests) {
      return Error(ReasonCode::QueueFull,
                   "the ingest queue already holds " + std::to_string(pending_.size()) +
                       " request(s), which is the configured bound");
    }
    pending_.push_back(std::move(request));
  }
  queue_cv_.notify_one();
  return future;
}

Status Observatory::publish_locked_out() {
  if (log_ == nullptr) {
    return fail(ReasonCode::InternalInvariant, "the runtime has no open evidence log");
  }

  Revision next = revision_;
  const std::optional<Revision> successor = revision_.successor();
  if (!successor.has_value()) {
    return fail(ReasonCode::ArithmeticOverflow, "snapshot revision counter is exhausted");
  }
  next = *successor;

  const Clock& clock = options_.clock != nullptr ? *options_.clock : SystemClock::instance();
  Result<Snapshot> built = Snapshot::build(next, log_->evidence(), options_.topology, options_.policy,
                                           clock.wall_now(), clock.steady_now());
  if (!built) {
    const std::lock_guard<std::mutex> guard(lifecycle_mutex_);
    state_ = RuntimeState::Failed;
    return built.error();
  }

  SnapshotHandle handle = std::make_shared<const Snapshot>(std::move(built).value());
  {
    const std::lock_guard<std::mutex> guard(publication_mutex_);
    published_ = handle;
    revision_ = next;
  }
  // Subscribers run outside every lock, on the worker thread.
  notify_subscribers(*handle);
  return ok_status();
}

Status Observatory::republish() { return publish_locked_out(); }

void Observatory::notify_subscribers(const Snapshot& snapshot) noexcept {
  std::vector<Subscriber> snapshot_of_subscribers;
  {
    const std::lock_guard<std::mutex> guard(subscribers_mutex_);
    snapshot_of_subscribers = subscribers_;
  }
  for (const Subscriber& subscriber : snapshot_of_subscribers) {
    if (!subscriber.callback) {
      continue;
    }
    try {
      subscriber.callback(snapshot);
    } catch (const std::exception&) {
      callback_failures_.fetch_add(1);
    } catch (...) {
      callback_failures_.fetch_add(1);
    }
  }
}

SnapshotHandle Observatory::current_snapshot() const {
  const std::lock_guard<std::mutex> guard(publication_mutex_);
  return published_;
}

Revision Observatory::current_revision() const {
  const std::lock_guard<std::mutex> guard(publication_mutex_);
  return revision_;
}

Result<Subscription> Observatory::subscribe(SnapshotCallback callback) {
  if (!callback) {
    return Error(ReasonCode::InvalidArgument, "subscription requires a callable");
  }
  Subscription subscription;
  {
    const std::lock_guard<std::mutex> guard(subscribers_mutex_);
    subscription.id = ++next_subscription_id_;
    subscribers_.push_back(Subscriber{subscription.id, std::move(callback)});
  }
  return subscription;
}

Status Observatory::unsubscribe(Subscription subscription) {
  const std::lock_guard<std::mutex> guard(subscribers_mutex_);
  const auto found = std::find_if(subscribers_.begin(), subscribers_.end(),
                                  [subscription](const Subscriber& entry) { return entry.id == subscription.id; });
  if (found == subscribers_.end()) {
    return fail(ReasonCode::NotFound,
                "subscription " + std::to_string(subscription.id) + " is not registered");
  }
  subscribers_.erase(found);
  return ok_status();
}

Status Observatory::advance_epoch(Epoch epoch, std::string reason) {
  if (log_ == nullptr) {
    return fail(ReasonCode::InternalInvariant, "the runtime has no open evidence log");
  }
  const Status advanced = log_->advance_epoch(epoch, std::move(reason));
  if (!advanced) {
    return advanced;
  }
  return republish();
}

Epoch Observatory::epoch() const {
  if (log_ == nullptr) {
    return Epoch{};
  }
  return log_->epoch();
}

const RecoveryReport& Observatory::recovery() const {
  static const RecoveryReport empty;
  return log_ == nullptr ? empty : log_->recovery();
}

std::size_t Observatory::pending_request_count() const {
  const std::lock_guard<std::mutex> guard(queue_mutex_);
  return pending_.size();
}

std::size_t Observatory::subscriber_count() const {
  const std::lock_guard<std::mutex> guard(subscribers_mutex_);
  return subscribers_.size();
}

IngestResult Observatory::last_ingest_result() const {
  const std::lock_guard<std::mutex> guard(result_mutex_);
  return last_result_;
}

}  // namespace po
