// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/file_lock.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <string>

namespace po {
namespace {

// One representation of ownership on both platforms, so the class body does not
// need two code paths for hold/release.
struct LockHandle {
#ifdef _WIN32
  void* handle{nullptr};
#else
  int descriptor{-1};
#endif
};

#ifdef _WIN32
[[nodiscard]] bool widen(const std::string& utf8, std::wstring& out) {
  out.clear();
  if (utf8.empty()) {
    return true;
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (size <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                               out.data(), size) == size;
}
#endif

enum class LockMode : std::uint8_t { Shared = 0, Exclusive = 1 };

[[nodiscard]] Result<LockHandle> open_lock_file(const std::string& path, LockMode mode, bool wait) {
  LockHandle result;
#ifdef _WIN32
  std::wstring wide;
  if (!widen(path, wide)) {
    return Error(ReasonCode::PathInvalid, "lock path is not valid UTF-8: " + path);
  }
  const HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    return Error(ReasonCode::LockFailed,
                 "could not open lock file '" + path + "' (windows error " + std::to_string(code) + ")");
  }
  OVERLAPPED overlapped{};
  DWORD flags = mode == LockMode::Exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0;
  if (!wait) {
    flags |= LOCKFILE_FAIL_IMMEDIATELY;
  }
  if (!::LockFileEx(handle, flags, 0, 1, 0, &overlapped)) {
    const DWORD code = ::GetLastError();
    static_cast<void>(::CloseHandle(handle));
    if (code == ERROR_LOCK_VIOLATION) {
      return Error(ReasonCode::WriterLockHeld,
                   "another process already holds the exclusive writer lock on '" + path + "'");
    }
    return Error(ReasonCode::LockFailed,
                 "could not take the writer lock on '" + path + "' (windows error " + std::to_string(code) + ")");
  }
  result.handle = handle;
  return result;
#else
  const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (descriptor < 0) {
    return Error(ReasonCode::LockFailed, "could not open lock file '" + path + "'");
  }
  const int operation = (mode == LockMode::Exclusive ? LOCK_EX : LOCK_SH) | (wait ? 0 : LOCK_NB);
  if (::flock(descriptor, operation) != 0) {
    const int saved = errno;
    static_cast<void>(::close(descriptor));
    if (saved == EWOULDBLOCK || saved == EAGAIN) {
      return Error(ReasonCode::WriterLockHeld,
                   "another process already holds the exclusive writer lock on '" + path + "'");
    }
    return Error(ReasonCode::LockFailed, "could not take the writer lock on '" + path + "'");
  }
  result.descriptor = descriptor;
  return result;
#endif
}

void close_lock_file(LockHandle& handle) noexcept {
#ifdef _WIN32
  if (handle.handle != nullptr) {
    OVERLAPPED overlapped{};
    static_cast<void>(::UnlockFileEx(handle.handle, 0, 1, 0, &overlapped));
    static_cast<void>(::CloseHandle(handle.handle));
    handle.handle = nullptr;
  }
#else
  if (handle.descriptor >= 0) {
    static_cast<void>(::flock(handle.descriptor, LOCK_UN));
    static_cast<void>(::close(handle.descriptor));
    handle.descriptor = -1;
  }
#endif
}

}  // namespace

FileLock::~FileLock() { release(); }

FileLock::FileLock(FileLock&& other) noexcept : path_(std::move(other.path_)) {
#ifdef _WIN32
  handle_ = other.handle_;
  other.handle_ = nullptr;
#else
  descriptor_ = other.descriptor_;
  other.descriptor_ = -1;
#endif
}

FileLock& FileLock::operator=(FileLock&& other) noexcept {
  if (this != &other) {
    release();
    path_ = std::move(other.path_);
#ifdef _WIN32
    handle_ = other.handle_;
    other.handle_ = nullptr;
#else
    descriptor_ = other.descriptor_;
    other.descriptor_ = -1;
#endif
  }
  return *this;
}

bool FileLock::held() const noexcept {
#ifdef _WIN32
  return handle_ != nullptr;
#else
  return descriptor_ >= 0;
#endif
}

void FileLock::release() noexcept {
#ifdef _WIN32
  LockHandle handle;
  handle.handle = handle_;
  handle_ = nullptr;
  close_lock_file(handle);
#else
  LockHandle handle;
  handle.descriptor = descriptor_;
  descriptor_ = -1;
  close_lock_file(handle);
#endif
}

Result<FileLock> FileLock::try_acquire_exclusive(const std::string& path) {
  Result<LockHandle> handle = open_lock_file(path, LockMode::Exclusive, false);
  if (!handle) {
    return handle.error();
  }
  FileLock lock;
  lock.path_ = path;
#ifdef _WIN32
  lock.handle_ = handle.value().handle;
#else
  lock.descriptor_ = handle.value().descriptor;
#endif
  return lock;
}

Result<FileLock> FileLock::acquire_exclusive(const std::string& path) {
  Result<LockHandle> handle = open_lock_file(path, LockMode::Exclusive, true);
  if (!handle) {
    return handle.error();
  }
  FileLock lock;
  lock.path_ = path;
#ifdef _WIN32
  lock.handle_ = handle.value().handle;
#else
  lock.descriptor_ = handle.value().descriptor;
#endif
  return lock;
}

Result<FileLock> FileLock::try_acquire_shared(const std::string& path) {
  Result<LockHandle> handle = open_lock_file(path, LockMode::Shared, false);
  if (!handle) {
    return handle.error();
  }
  FileLock lock;
  lock.path_ = path;
#ifdef _WIN32
  lock.handle_ = handle.value().handle;
#else
  lock.descriptor_ = handle.value().descriptor;
#endif
  return lock;
}

}  // namespace po
