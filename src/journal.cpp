#include "journal.hpp"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <system_error>
#include <unistd.h>

namespace minivector {
namespace {

constexpr std::array<std::uint8_t, Journal::records_begin> magic{
    'M', 'V', 'D', 'B', 'L', 'O', 'G', '1'};
constexpr std::uint64_t frame_header_size = 8;

[[noreturn]] void io_failure(const char *description) {
  throw std::system_error(errno ? errno : EIO, std::generic_category(), description);
}

std::FILE *open_journal(const std::filesystem::path &path) {
  const int descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
  if (descriptor < 0) {
    io_failure("open journal");
  }
  if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int saved_errno = errno;
    ::close(descriptor);
    errno = saved_errno;
    io_failure("lock journal");
  }
  std::FILE *file = ::fdopen(descriptor, "r+b");
  if (file == nullptr) {
    const int saved_errno = errno;
    ::close(descriptor);
    errno = saved_errno;
    io_failure("open journal stream");
  }
  return file;
}

void seek_file(std::FILE *file, std::uint64_t offset, int origin) {
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    throw std::length_error("journal exceeds the file offset range");
  }
  const int result = ::fseeko(file, static_cast<off_t>(offset), origin);
  if (result != 0) {
    io_failure("seek journal");
  }
}

std::uint64_t file_position(std::FILE *file) {
  const auto position = ::ftello(file);
  if (position < 0) {
    io_failure("read journal position");
  }
  return static_cast<std::uint64_t>(position);
}

std::uint32_t checksum(const Bytes &bytes) {
  static const auto table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t i = 0; i < values.size(); ++i) {
      std::uint32_t value = i;
      for (int bit = 0; bit < 8; ++bit) {
        value = (value >> 1) ^ ((value & 1) != 0 ? 0xedb88320U : 0U);
      }
      values[i] = value;
    }
    return values;
  }();
  std::uint32_t crc = 0xffffffffU;
  for (const std::uint8_t value : bytes) {
    crc = (crc >> 8) ^ table[(crc ^ value) & 0xff];
  }
  return crc ^ 0xffffffffU;
}

}  // namespace

Journal::Journal(const std::filesystem::path &directory) {
  std::filesystem::create_directories(directory);
  stream_.reset(open_journal(directory / "store.wal"));
  if (std::setvbuf(stream_.get(), buffer_.data(), _IOFBF, buffer_.size()) != 0) {
    io_failure("set journal buffer");
  }
  seek_file(stream_.get(), 0, SEEK_END);
  position_ = size_ = file_position(stream_.get());

  std::array<std::uint8_t, magic.size()> stored{};
  const auto prefix = static_cast<std::size_t>(std::min<std::uint64_t>(size_, magic.size()));
  if (prefix != 0) {
    read_bytes(0, stored.data(), prefix);
    if (!std::equal(stored.begin(), stored.begin() + prefix, magic.begin())) {
      throw std::runtime_error("unsupported journal format");
    }
  }
  if (prefix < magic.size()) {
    truncate(0);
    append_bytes(magic.data(), magic.size());
    sync();
  }
}

void Journal::position(std::uint64_t offset, Access access) {
  if (position_ != offset || access_ != access) {
    seek_file(stream_.get(), offset, SEEK_SET);
    position_ = offset;
    access_ = access;
  }
}

void Journal::read_bytes(std::uint64_t offset, void *destination, std::size_t size) {
  if (offset > size_ || size > size_ - offset) {
    throw std::runtime_error("read outside journal bounds");
  }
  position(offset, Access::Read);
  if (std::fread(destination, 1, size, stream_.get()) != size) {
    io_failure("read journal");
  }
  position_ += size;
}

void Journal::append_bytes(const void *source, std::size_t size) {
  position(size_, Access::Write);
  if (std::fwrite(source, 1, size, stream_.get()) != size) {
    io_failure("append journal");
  }
  size_ += size;
  position_ += size;
}

RecordRef Journal::append(const Bytes &payload) {
  if (payload.empty() || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("journal record size is out of range");
  }
  const auto length = static_cast<std::uint32_t>(payload.size());
  const auto crc = checksum(payload);
  std::array<std::uint8_t, frame_header_size> header{};
  for (std::size_t i = 0; i < sizeof(length); ++i) {
    header[i] = static_cast<std::uint8_t>(length >> (8 * i));
    header[i + sizeof(length)] = static_cast<std::uint8_t>(crc >> (8 * i));
  }
  append_bytes(header.data(), header.size());
  const RecordRef record{size_, length};
  append_bytes(payload.data(), payload.size());
  return record;
}

void Journal::read(RecordRef record, Bytes &out) {
  out.resize(record.size);
  read_bytes(record.offset, out.data(), out.size());
}

bool Journal::read_frame(std::uint64_t offset, RecordRef &record, Bytes &out) {
  if (offset > size_ || size_ - offset < frame_header_size) {
    return false;
  }
  Bytes header(frame_header_size);
  read_bytes(offset, header.data(), header.size());
  Reader reader(header);
  const std::uint32_t length = reader.u32();
  const std::uint32_t crc = reader.u32();
  if (length == 0 || length > size_ - offset - frame_header_size) {
    return false;
  }
  record = {offset + frame_header_size, length};
  read(record, out);
  return checksum(out) == crc;
}

void Journal::truncate(std::uint64_t size) {
  if (size > size_) {
    throw std::runtime_error("cannot extend journal during rollback");
  }
  seek_file(stream_.get(), size, SEEK_SET);
  if (::ftruncate(::fileno(stream_.get()), static_cast<off_t>(size)) != 0) {
    io_failure("truncate journal");
  }
  size_ = position_ = size;
  access_ = Access::None;
}

void Journal::sync() {
  if (access_ == Access::Write && std::fflush(stream_.get()) != 0) {
    io_failure("flush journal");
  }
  int result = 0;
  do {
    result = ::fdatasync(::fileno(stream_.get()));
  } while (result != 0 && errno == EINTR);
  if (result != 0) {
    io_failure("sync journal");
  }
}

}  // namespace minivector
