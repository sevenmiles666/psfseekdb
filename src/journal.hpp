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

class JournalView {
public:
  JournalView() = default;
  JournalView(const void *address, std::size_t length);
  explicit operator bool() const { return static_cast<bool>(data_); }
  Reader read(RecordRef record) const;

private:
  struct Unmap {
    std::size_t length;
    void operator()(const std::uint8_t *address) const noexcept;
  };
  std::unique_ptr<const std::uint8_t, Unmap> data_{nullptr, Unmap{0}};
};

class Journal {
public:
  explicit Journal(const std::filesystem::path &directory, const char *filename = "store.wal");

  static constexpr std::uint64_t records_begin = 8;
  static constexpr std::uint64_t frame_header_size = 8;
  std::uint64_t size() const { return size_; }
  RecordRef append(const Bytes &payload);
  void read(RecordRef record, Bytes &out);
  bool read_frame(std::uint64_t offset, RecordRef &record, Bytes &out);
  void truncate(std::uint64_t size);
  void sync();
  JournalView view();
  void replace_with(Journal &replacement, const std::filesystem::path &directory);

private:
  enum class Access { None, Read, Write };
  void position(std::uint64_t offset, Access access);
  void read_bytes(std::uint64_t offset, void *destination, std::size_t size);
  void append_bytes(const void *source, std::size_t size);

  // Declared before the stream so it remains alive until fclose finishes.
  std::unique_ptr<std::FILE, int (*)(std::FILE *)> directory_lock_{nullptr, std::fclose};
  std::unique_ptr<std::array<char, 64 * 1024>> buffer_ =
      std::make_unique<std::array<char, 64 * 1024>>();
  std::unique_ptr<std::FILE, int (*)(std::FILE *)> stream_{nullptr, std::fclose};
  std::uint64_t size_ = 0;
  std::uint64_t position_ = 0;
  Access access_ = Access::None;
};

}  // namespace minivector
