// Power Observatory - electrical observability runtime for data center control planes.
// Copyright 2026 Summon Software Labs. Apache License 2.0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "power_observatory/result.hpp"

namespace po {

// Byte-oriented file access with an explicit durability control. Nothing here
// returns a sentinel on failure: every operation either succeeds or reports a
// reason code, so a caller can never mistake a failed write for a short one.
class FileHandle {
 public:
  FileHandle() = default;
  ~FileHandle();
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  [[nodiscard]] static Result<FileHandle> open_read(const std::string& path);
  // Opens for appending, creating the file when it does not exist.
  [[nodiscard]] static Result<FileHandle> open_append(const std::string& path);
  [[nodiscard]] static Result<FileHandle> create_new(const std::string& path, bool truncate_existing);
  // Opens an existing file for reading and writing without changing its
  // contents. Required by recovery, which may need to truncate a torn tail.
  [[nodiscard]] static Result<FileHandle> open_read_write(const std::string& path);

  [[nodiscard]] Status read_at(std::uint64_t offset, void* buffer, std::size_t size, std::size_t& bytes_read);
  [[nodiscard]] Status append(const void* buffer, std::size_t size);
  [[nodiscard]] Status position_at_end();
  [[nodiscard]] Result<std::uint64_t> size() const;
  [[nodiscard]] Status truncate_to(std::uint64_t length);
  // Forces every byte written so far to stable storage. For an append-only log
  // this is the commit point of the record that was just written.
  [[nodiscard]] Status sync();
  [[nodiscard]] Status close();
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  std::string path_;
#ifdef _WIN32
  void* handle_{nullptr};
#else
  int descriptor_{-1};
#endif
};

[[nodiscard]] Result<std::vector<std::uint8_t>> read_whole_file(const std::string& path);
// Writes to a sibling temporary file, forces that file to stable storage, and
// renames it over the destination, so a concurrent reader observes either the
// old content or the new content and never a partial file.
[[nodiscard]] Status write_file_atomically(const std::string& path, const void* data, std::size_t size);
[[nodiscard]] Status write_file_atomically(const std::string& path, std::string_view text);

[[nodiscard]] Status remove_file(const std::string& path) noexcept;
[[nodiscard]] bool file_exists(const std::string& path) noexcept;
[[nodiscard]] Result<std::uint64_t> file_size(const std::string& path);
[[nodiscard]] Status create_directories(const std::string& path);
[[nodiscard]] Status remove_directory_recursively(const std::string& path) noexcept;
[[nodiscard]] std::string join_path(std::string_view directory, std::string_view name);
[[nodiscard]] std::string parent_directory(const std::string& path);
[[nodiscard]] Result<std::string> current_directory();
[[nodiscard]] Result<std::string> absolute_path(const std::string& path);
// A unique sibling path used for temporary and lock files.
[[nodiscard]] std::string unique_sibling_path(const std::string& path, std::string_view tag);

}  // namespace po
