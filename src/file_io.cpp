// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#include "power_observatory/file_io.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace po {
namespace {

[[nodiscard]] std::filesystem::path to_path(const std::string& utf8) {
  // The public surface is UTF-8 on every platform. On Windows the path must be
  // built from a char8_t sequence so that std::filesystem performs the correct
  // UTF-8 to wide conversion instead of interpreting the bytes as the ANSI code
  // page. On POSIX the native narrow encoding is already UTF-8, so the bytes are
  // handed over directly and no char8_t dependency is needed at all.
#ifdef _WIN32
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#else
  return std::filesystem::path(utf8);
#endif
}

[[nodiscard]] std::uint64_t process_identifier() noexcept {
#ifdef _WIN32
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::uint64_t next_unique_counter() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

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
  const int converted = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                              static_cast<int>(utf8.size()), out.data(), size);
  return converted == size;
}

[[nodiscard]] ReasonCode classify_windows_error(DWORD code) noexcept {
  switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
      return ReasonCode::NotFound;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return ReasonCode::LockFailed;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return ReasonCode::StorageExhausted;
    default:
      return ReasonCode::IoError;
  }
}
#endif

}  // namespace

FileHandle::~FileHandle() {
  if (is_open()) {
    static_cast<void>(close());
  }
}

FileHandle::FileHandle(FileHandle&& other) noexcept
    : path_(std::move(other.path_)) {
#ifdef _WIN32
  handle_ = other.handle_;
  other.handle_ = nullptr;
#else
  descriptor_ = other.descriptor_;
  other.descriptor_ = -1;
#endif
}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    if (is_open()) {
      static_cast<void>(close());
    }
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

bool FileHandle::is_open() const noexcept {
#ifdef _WIN32
  return handle_ != nullptr;
#else
  return descriptor_ >= 0;
#endif
}

Result<FileHandle> FileHandle::open_read(const std::string& path) {
#ifdef _WIN32
  std::wstring wide;
  if (!widen(path, wide)) {
    return Error(ReasonCode::PathInvalid, "path is not valid UTF-8: " + path);
  }
  const HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    return Error(classify_windows_error(code),
                 "could not open '" + path + "' for reading (windows error " + std::to_string(code) + ")");
  }
  FileHandle result;
  result.handle_ = handle;
  result.path_ = path;
  return result;
#else
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return Error(ReasonCode::OpenFailed, "could not open '" + path + "' for reading");
  }
  FileHandle result;
  result.descriptor_ = descriptor;
  result.path_ = path;
  return result;
#endif
}

Result<FileHandle> FileHandle::open_append(const std::string& path) {
#ifdef _WIN32
  std::wstring wide;
  if (!widen(path, wide)) {
    return Error(ReasonCode::PathInvalid, "path is not valid UTF-8: " + path);
  }
  // FILE_APPEND_DATA with a null OVERLAPPED gives atomic append semantics.
  const HANDLE handle = ::CreateFileW(wide.c_str(), FILE_APPEND_DATA | GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    return Error(classify_windows_error(code),
                 "could not open '" + path + "' for appending (windows error " + std::to_string(code) + ")");
  }
  FileHandle result;
  result.handle_ = handle;
  result.path_ = path;
  return result;
#else
  const int descriptor = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
  if (descriptor < 0) {
    return Error(ReasonCode::OpenFailed, "could not open '" + path + "' for appending");
  }
  FileHandle result;
  result.descriptor_ = descriptor;
  result.path_ = path;
  return result;
#endif
}

Result<FileHandle> FileHandle::create_new(const std::string& path, bool truncate_existing) {
#ifdef _WIN32
  std::wstring wide;
  if (!widen(path, wide)) {
    return Error(ReasonCode::PathInvalid, "path is not valid UTF-8: " + path);
  }
  const HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      truncate_existing ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    return Error(classify_windows_error(code),
                 "could not create '" + path + "' (windows error " + std::to_string(code) + ")");
  }
  FileHandle result;
  result.handle_ = handle;
  result.path_ = path;
  return result;
#else
  const int flags = O_RDWR | O_CREAT | (truncate_existing ? O_TRUNC : O_EXCL);
  const int descriptor = ::open(path.c_str(), flags, 0644);
  if (descriptor < 0) {
    return Error(ReasonCode::OpenFailed, "could not create '" + path + "'");
  }
  FileHandle result;
  result.descriptor_ = descriptor;
  result.path_ = path;
  return result;
#endif
}

Result<FileHandle> FileHandle::open_read_write(const std::string& path) {
#ifdef _WIN32
  std::wstring wide;
  if (!widen(path, wide)) {
    return Error(ReasonCode::PathInvalid, "path is not valid UTF-8: " + path);
  }
  const HANDLE handle = ::CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    return Error(classify_windows_error(code),
                 "could not open '" + path + "' for reading and writing (windows error " +
                     std::to_string(code) + ")");
  }
  FileHandle result;
  result.handle_ = handle;
  result.path_ = path;
  return result;
#else
  const int descriptor = ::open(path.c_str(), O_RDWR);
  if (descriptor < 0) {
    return Error(ReasonCode::OpenFailed, "could not open '" + path + "' for reading and writing");
  }
  FileHandle result;
  result.descriptor_ = descriptor;
  result.path_ = path;
  return result;
#endif
}

Status FileHandle::read_at(std::uint64_t offset, void* buffer, std::size_t size, std::size_t& bytes_read) {
  bytes_read = 0;
  if (!is_open()) {
    return fail(ReasonCode::ReadFailed, "read attempted on a closed handle for '" + path_ + "'");
  }
  if (size == 0) {
    return ok_status();
  }
#ifdef _WIN32
  if (size > 0xFFFFFFFFull) {
    return fail(ReasonCode::InvalidArgument, "read size exceeds the platform limit");
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  DWORD transferred = 0;
  if (!::ReadFile(handle_, buffer, static_cast<DWORD>(size), &transferred, &overlapped)) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_HANDLE_EOF) {
      bytes_read = 0;
      return ok_status();
    }
    return fail(classify_windows_error(code),
                "read from '" + path_ + "' at offset " + std::to_string(offset) +
                    " failed (windows error " + std::to_string(code) + ")");
  }
  bytes_read = transferred;
  return ok_status();
#else
  std::size_t total = 0;
  while (total < size) {
    const ssize_t taken = ::pread(descriptor_, static_cast<unsigned char*>(buffer) + total, size - total,
                                  static_cast<off_t>(offset + total));
    if (taken < 0) {
      return fail(ReasonCode::ReadFailed, "read from '" + path_ + "' failed");
    }
    if (taken == 0) {
      break;
    }
    total += static_cast<std::size_t>(taken);
  }
  bytes_read = total;
  return ok_status();
#endif
}

Status FileHandle::append(const void* buffer, std::size_t size) {
  if (!is_open()) {
    return fail(ReasonCode::WriteFailed, "append attempted on a closed handle for '" + path_ + "'");
  }
  if (size == 0) {
    return ok_status();
  }
#ifdef _WIN32
  if (size > 0xFFFFFFFFull) {
    return fail(ReasonCode::InvalidArgument, "append size exceeds the platform limit");
  }
  DWORD transferred = 0;
  if (!::WriteFile(handle_, buffer, static_cast<DWORD>(size), &transferred, nullptr)) {
    const DWORD code = ::GetLastError();
    return fail(classify_windows_error(code),
                "append to '" + path_ + "' failed (windows error " + std::to_string(code) + ")");
  }
  if (transferred != size) {
    return fail(ReasonCode::WriteFailed,
                "append to '" + path_ + "' wrote " + std::to_string(transferred) + " of " +
                    std::to_string(size) + " bytes");
  }
  return ok_status();
#else
  std::size_t total = 0;
  while (total < size) {
    const ssize_t written = ::write(descriptor_, static_cast<const unsigned char*>(buffer) + total, size - total);
    if (written <= 0) {
      return fail(ReasonCode::WriteFailed, "append to '" + path_ + "' failed");
    }
    total += static_cast<std::size_t>(written);
  }
  return ok_status();
#endif
}

Status FileHandle::position_at_end() {
  if (!is_open()) {
    return fail(ReasonCode::InvalidArgument, "seek attempted on a closed handle for '" + path_ + "'");
  }
#ifdef _WIN32
  LARGE_INTEGER distance{};
  if (!::SetFilePointerEx(handle_, distance, nullptr, FILE_END)) {
    return fail(ReasonCode::IoError, "could not position '" + path_ + "' at end of file");
  }
  return ok_status();
#else
  if (::lseek(descriptor_, 0, SEEK_END) < 0) {
    return fail(ReasonCode::IoError, "could not position '" + path_ + "' at end of file");
  }
  return ok_status();
#endif
}

Result<std::uint64_t> FileHandle::size() const {
  if (!is_open()) {
    return Error(ReasonCode::InvalidArgument, "size requested from a closed handle for '" + path_ + "'");
  }
#ifdef _WIN32
  LARGE_INTEGER length{};
  if (!::GetFileSizeEx(handle_, &length)) {
    return Error(ReasonCode::IoError, "could not determine the size of '" + path_ + "'");
  }
  return static_cast<std::uint64_t>(length.QuadPart);
#else
  struct stat information{};
  if (::fstat(descriptor_, &information) != 0) {
    return Error(ReasonCode::IoError, "could not determine the size of '" + path_ + "'");
  }
  return static_cast<std::uint64_t>(information.st_size);
#endif
}

Status FileHandle::truncate_to(std::uint64_t length) {
  if (!is_open()) {
    return fail(ReasonCode::InvalidArgument, "truncate attempted on a closed handle for '" + path_ + "'");
  }
#ifdef _WIN32
  LARGE_INTEGER distance{};
  distance.QuadPart = static_cast<LONGLONG>(length);
  if (!::SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN)) {
    return fail(ReasonCode::IoError, "could not position '" + path_ + "' for truncation");
  }
  if (!::SetEndOfFile(handle_)) {
    const DWORD code = ::GetLastError();
    return fail(classify_windows_error(code),
                "could not truncate '" + path_ + "' (windows error " + std::to_string(code) + ")");
  }
  return ok_status();
#else
  if (::ftruncate(descriptor_, static_cast<off_t>(length)) != 0) {
    return fail(ReasonCode::IoError, "could not truncate '" + path_ + "'");
  }
  return ok_status();
#endif
}

Status FileHandle::sync() {
  if (!is_open()) {
    return fail(ReasonCode::InvalidArgument, "sync attempted on a closed handle for '" + path_ + "'");
  }
#ifdef _WIN32
  if (!::FlushFileBuffers(handle_)) {
    const DWORD code = ::GetLastError();
    return fail(classify_windows_error(code),
                "could not flush '" + path_ + "' to stable storage (windows error " + std::to_string(code) + ")");
  }
  return ok_status();
#else
  if (::fsync(descriptor_) != 0) {
    return fail(ReasonCode::IoError, "could not flush '" + path_ + "' to stable storage");
  }
  return ok_status();
#endif
}

Status FileHandle::close() {
  if (!is_open()) {
    return ok_status();
  }
#ifdef _WIN32
  const HANDLE handle = handle_;
  handle_ = nullptr;
  if (!::CloseHandle(handle)) {
    return fail(ReasonCode::IoError, "could not close '" + path_ + "'");
  }
  return ok_status();
#else
  const int descriptor = descriptor_;
  descriptor_ = -1;
  if (::close(descriptor) != 0) {
    return fail(ReasonCode::IoError, "could not close '" + path_ + "'");
  }
  return ok_status();
#endif
}

Result<std::vector<std::uint8_t>> read_whole_file(const std::string& path) {
  Result<FileHandle> handle = FileHandle::open_read(path);
  if (!handle) {
    return handle.error();
  }
  const Result<std::uint64_t> length = handle.value().size();
  if (!length) {
    return length.error();
  }
  if (length.value() > static_cast<std::uint64_t>(64) * 1024 * 1024) {
    return Error(ReasonCode::StorageExhausted,
                 "'" + path + "' is larger than the 64 MiB bound this runtime will read into memory");
  }
  std::vector<std::uint8_t> buffer(static_cast<std::size_t>(length.value()));
  if (buffer.empty()) {
    return buffer;
  }
  std::size_t transferred = 0;
  const Status status = handle.value().read_at(0, buffer.data(), buffer.size(), transferred);
  if (!status) {
    return status.error();
  }
  if (transferred != buffer.size()) {
    return Error(ReasonCode::ReadFailed, "short read from '" + path + "'");
  }
  return buffer;
}

Status write_file_atomically(const std::string& path, const void* data, std::size_t size) {
  const std::string temporary = unique_sibling_path(path, "tmp");
  Status status = ok_status();
  {
    Result<FileHandle> handle = FileHandle::create_new(temporary, true);
    if (!handle) {
      return handle.error();
    }
    status = handle.value().append(data, size);
    if (status) {
      status = handle.value().sync();
    }
    const Status closed = handle.value().close();
    if (status && !closed) {
      status = closed;
    }
  }
  if (!status) {
    static_cast<void>(remove_file(temporary));
    return status;
  }

#ifdef _WIN32
  std::wstring wide_source;
  std::wstring wide_target;
  if (!widen(temporary, wide_source) || !widen(path, wide_target)) {
    static_cast<void>(remove_file(temporary));
    return fail(ReasonCode::PathInvalid, "path is not valid UTF-8: " + path);
  }
  if (!::MoveFileExW(wide_source.c_str(), wide_target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    const DWORD code = ::GetLastError();
    static_cast<void>(remove_file(temporary));
    return fail(classify_windows_error(code),
                "could not publish '" + path + "' (windows error " + std::to_string(code) + ")");
  }
  return ok_status();
#else
  if (::rename(temporary.c_str(), path.c_str()) != 0) {
    static_cast<void>(remove_file(temporary));
    return fail(ReasonCode::IoError, "could not publish '" + path + "'");
  }
  return ok_status();
#endif
}

Status write_file_atomically(const std::string& path, std::string_view text) {
  return write_file_atomically(path, text.data(), text.size());
}

Status remove_file(const std::string& path) noexcept {
  std::error_code error;
  static_cast<void>(std::filesystem::remove(to_path(path), error));
  if (error) {
    std::error_code existence_error;
    // A missing file is not a failure: removal is idempotent.
    if (!std::filesystem::exists(to_path(path), existence_error)) {
      return ok_status();
    }
    return fail(ReasonCode::IoError, "could not remove '" + path + "': " + error.message());
  }
  return ok_status();
}

bool file_exists(const std::string& path) noexcept {
  std::error_code error;
  return std::filesystem::exists(to_path(path), error) && !error;
}

Result<std::uint64_t> file_size(const std::string& path) {
  std::error_code error;
  const std::uintmax_t length = std::filesystem::file_size(to_path(path), error);
  if (error) {
    return Error(ReasonCode::NotFound, "could not determine the size of '" + path + "': " + error.message());
  }
  return static_cast<std::uint64_t>(length);
}

Status create_directories(const std::string& path) {
  if (path.empty()) {
    return fail(ReasonCode::PathInvalid, "directory path is empty");
  }
  std::error_code error;
  std::filesystem::create_directories(to_path(path), error);
  if (error) {
    return fail(ReasonCode::IoError, "could not create directory '" + path + "': " + error.message());
  }
  return ok_status();
}

Status remove_directory_recursively(const std::string& path) noexcept {
  std::error_code error;
  std::filesystem::remove_all(to_path(path), error);
  if (error) {
    return fail(ReasonCode::IoError, "could not remove directory '" + path + "': " + error.message());
  }
  return ok_status();
}

std::string join_path(std::string_view directory, std::string_view name) {
  if (directory.empty()) {
    return std::string(name);
  }
  const std::filesystem::path combined = to_path(std::string(directory)) / to_path(std::string(name));
  return combined.string();
}

std::string parent_directory(const std::string& path) {
  const std::filesystem::path parent = to_path(path).parent_path();
  return parent.empty() ? std::string(".") : parent.string();
}

Result<std::string> current_directory() {
  std::error_code error;
  const std::filesystem::path directory = std::filesystem::current_path(error);
  if (error) {
    return Error(ReasonCode::IoError, "could not determine the current directory: " + error.message());
  }
  return directory.string();
}

Result<std::string> absolute_path(const std::string& path) {
  std::error_code error;
  const std::filesystem::path absolute = std::filesystem::absolute(to_path(path), error);
  if (error) {
    return Error(ReasonCode::PathInvalid, "could not resolve '" + path + "': " + error.message());
  }
  return absolute.lexically_normal().string();
}

std::string unique_sibling_path(const std::string& path, std::string_view tag) {
  std::string suffix;
  suffix.append(".");
  suffix.append(tag);
  suffix.append(".");
  suffix.append(std::to_string(process_identifier()));
  suffix.append(".");
  suffix.append(std::to_string(next_unique_counter()));
  return path + suffix;
}

}  // namespace po
