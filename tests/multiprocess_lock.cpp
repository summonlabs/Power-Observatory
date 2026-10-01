// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
//
// Real cross-process exclusion. A second process takes the writer lock and this
// process is required to be refused: not to wait, and not to proceed.
#include "test_harness.hpp"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "po_fixtures.hpp"
#include "power_observatory/file_io.hpp"
#include "power_observatory/file_lock.hpp"
#include "power_observatory/persistence.hpp"

using namespace po;

namespace {

#ifndef PO_CLI_PATH
#define PO_CLI_PATH "power-observatory"
#endif

// Waits for a file to appear. The poll only observes the child's own progress;
// it never decides whether the behaviour under test is correct, and the child
// is always released afterwards.
[[nodiscard]] bool await_file(const std::string& path) {
  for (int attempt = 0; attempt < 3000; ++attempt) {
    if (file_exists(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return false;
}

}  // namespace

PO_TEST(multiprocess, a_second_process_cannot_take_the_writer_lock) {
  const std::string directory = pofix::scratch("multiprocess_lock");
  const std::string store = join_path(directory, "site");
  const std::string ready = join_path(directory, "ready");
  const std::string release = join_path(directory, "release");
  const std::string log_path = join_path(store, "evidence.poev");

  PO_CHECK_OK(create_directories(store));
  JsonValue site = JsonValue::object();
  site.set("policy", JsonValue("default"));
  site.set("scenario", JsonValue("standard-2n"));
  site.set("schema_version", JsonValue(std::uint64_t{1}));
  site.set("seed", JsonValue(std::uint64_t{1}));
  PO_CHECK_OK(write_file_atomically(join_path(store, "site.json"), site.dump(true)));

  const pofix::ChildProcess holder =
      pofix::spawn_child(std::string(PO_CLI_PATH), {"hold", "--store", store, "--ready", ready, "--wait-for", release});
  PO_REQUIRE(holder.valid());

  const bool signalled = await_file(ready);
  if (!signalled) {
    // Release the holder so it cannot be left behind, then report.
    static_cast<void>(write_file_atomically(release, std::string_view("go")));
    static_cast<void>(pofix::wait_child(holder));
    PO_FAIL("the writer-lock holder never signalled readiness");
    pofix::clear("multiprocess_lock");
    return;
  }

  // The child holds the writer lock. This process must be refused.
  StoreOpenOptions options;
  const Result<EvidenceLog> denied = EvidenceLog::open(log_path, Epoch{}, options);
  PO_CHECK(!denied.has_value());
  PO_CHECK_EQ(denied.code(), ReasonCode::WriterLockHeld);

  // A read-only load is refused too, because a shared lock cannot coexist with
  // a writer that holds the exclusive lock.
  const Result<EvidenceLog::Loaded> read_denied = EvidenceLog::load(log_path, options);
  PO_CHECK(!read_denied.has_value());
  PO_CHECK_EQ(read_denied.code(), ReasonCode::WriterLockHeld);

  PO_CHECK_OK(write_file_atomically(release, std::string_view("go")));
  PO_CHECK_EQ(pofix::wait_child(holder), 0);

  // Once the holder has exited, the lock is available again.
  Result<EvidenceLog> granted = EvidenceLog::open(log_path, Epoch{}, options);
  PO_REQUIRE_OK(granted);
  PO_CHECK_OK(granted.value().close());
  pofix::clear("multiprocess_lock");
}

PO_TEST(multiprocess, an_exclusive_lock_excludes_another_exclusive_lock) {
  const std::string path = pofix::join("multiprocess_filelock", "guard.lock");

  Result<FileLock> first = FileLock::try_acquire_exclusive(path);
  PO_REQUIRE_OK(first);
  PO_CHECK(first.value().held());

  // A second attempt from the same process is refused as well: the operating
  // system grants one exclusive owner per file, not one per process.
  const Result<FileLock> second = FileLock::try_acquire_exclusive(path);
  PO_CHECK(!second.has_value());
  PO_CHECK_EQ(second.code(), ReasonCode::WriterLockHeld);

  // A shared lock is excluded while the exclusive lock is held.
  const Result<FileLock> shared = FileLock::try_acquire_shared(path);
  PO_CHECK(!shared.has_value());

  first.value().release();
  PO_CHECK(!first.value().held());
  Result<FileLock> after = FileLock::try_acquire_exclusive(path);
  PO_REQUIRE_OK(after);
  pofix::clear("multiprocess_filelock");
}

PO_TEST_MAIN()