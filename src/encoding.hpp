#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace minivector {

using Bytes = std::vector<std::uint8_t>;

inline void put_u32(Bytes &out, std::uint32_t value) {
  for (unsigned int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

inline void put_i64(Bytes &out, std::int64_t value) {
  const auto bits = static_cast<std::uint64_t>(value);
  for (unsigned int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(bits >> shift));
  }
}

inline void put_float(Bytes &out, float value) {
  static_assert(sizeof(float) == sizeof(std::uint32_t) && std::numeric_limits<float>::is_iec559,
                "the stored vector format requires IEEE float32");
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(out, bits);
}

inline void put_floats(Bytes &out, const float *values, std::size_t count) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
  if (count != 0) {
    const auto *start = reinterpret_cast<const std::uint8_t *>(values);
    out.insert(out.end(), start, start + count * sizeof(float));
  }
#else
  for (std::size_t i = 0; i < count; ++i) put_float(out, values[i]);
#endif
}

inline void put_string(Bytes &out, std::string_view value) {
  if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("value exceeds the on-disk string length range");
  }
  put_u32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

class Reader {
public:
  explicit Reader(const Bytes &input) : input_(input) {}

  std::size_t remaining() const { return input_.size() - position_; }

  void skip(std::size_t length) {
    require(length);
    position_ += length;
  }

  std::uint8_t byte() {
    require(1);
    return input_[position_++];
  }

  std::uint32_t u32() {
    require(4);
    std::uint32_t value = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8) {
      value |= static_cast<std::uint32_t>(input_[position_++]) << shift;
    }
    return value;
  }

  std::int64_t i64() {
    require(8);
    std::uint64_t bits = 0;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
      bits |= static_cast<std::uint64_t>(input_[position_++]) << shift;
    }
    std::int64_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  float f32() {
    const std::uint32_t bits = u32();
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }

  void floats(float *values, std::size_t count) {
    if (count > remaining() / sizeof(float)) {
      throw std::runtime_error("incomplete vector in journal");
    }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const auto length = count * sizeof(float);
    if (length != 0) std::memcpy(values, input_.data() + position_, length);
    position_ += length;
#else
    for (std::size_t i = 0; i < count; ++i) values[i] = f32();
#endif
  }

  std::string string() {
    const std::size_t length = u32();
    require(length);
    const auto *start = reinterpret_cast<const char *>(input_.data() + position_);
    position_ += length;
    return std::string(start, length);
  }

  void finish() const {
    if (remaining() != 0) {
      throw std::runtime_error("unexpected trailing bytes in a journal record");
    }
  }

private:
  void require(std::size_t length) const {
    if (length > remaining()) {
      throw std::runtime_error("incomplete value in a journal record");
    }
  }

  const Bytes &input_;
  std::size_t position_ = 0;
};

}  // namespace minivector
