#pragma once

#include "encoding.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <memory>

namespace minivector {

struct RecordRef {
  std::uint64_t offset;
  std::uint32_t size;
};

class Journal {
public:
  explicit Journal(const std::filesystem::path &directory);

  static constexpr std::uint64_t records_begin = 8;
  std::uint64_t size() const { return size_; }
  RecordRef append(const Bytes &payload);
  void read(RecordRef record, Bytes &out);
  bool read_frame(std::uint64_t offset, RecordRef &record, Bytes &out);
  void truncate(std::uint64_t size);
  void sync();

private:
  enum class Access { None, Read, Write };
  void position(std::uint64_t offset, Access access);
  void read_bytes(std::uint64_t offset, void *destination, std::size_t size);
  void append_bytes(const void *source, std::size_t size);

  // Declared before the stream so it remains alive until fclose finishes.
  std::array<char, 64 * 1024> buffer_{};
  std::unique_ptr<std::FILE, int (*)(std::FILE *)> stream_{nullptr, std::fclose};
  std::uint64_t size_ = 0;
  std::uint64_t position_ = 0;
  Access access_ = Access::None;
};

}  // namespace minivector
