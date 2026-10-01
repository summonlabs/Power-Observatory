// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <string>

#include "power_observatory/result.hpp"

namespace po {

// A kernel-enforced, advisory-free exclusive lock on a lock file.
//
// The lock is owned by the process, not by the thread, and the operating system
// releases it when the process dies for any reason, including abrupt
// termination. That is what makes it usable as a single-writer guard for the
// evidence log: a crashed writer never leaves a lock behind.
class FileLock {
 public:
  FileLock() = default;
  ~FileLock();
  FileLock(FileLock&& other) noexcept;
  FileLock& operator=(FileLock&& other) noexcept;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  // Attempts to take the lock without waiting. A lock held by another process
  // is reported as WriterLockHeld rather than blocking.
  [[nodiscard]] static Result<FileLock> try_acquire_exclusive(const std::string& path);
  // Waits for the lock. Used by tests that need to prove release-on-exit.
  [[nodiscard]] static Result<FileLock> acquire_exclusive(const std::string& path);
  // A shared lock, used by readers. Many readers may hold it at once; a writer
  // holding the exclusive lock excludes all of them.
  [[nodiscard]] static Result<FileLock> try_acquire_shared(const std::string& path);

  [[nodiscard]] bool held() const noexcept;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  void release() noexcept;

 private:
  std::string path_;
#ifdef _WIN32
  void* handle_{nullptr};
#else
  int descriptor_{-1};
#endif
};

}  // namespace po
