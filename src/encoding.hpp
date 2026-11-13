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

enum class VectorEncoding { Full, Half, Packed, ScaledByte };

inline std::uint32_t float_bits(float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

inline float float_from_bits(std::uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline std::size_t vector_bytes(VectorEncoding encoding, std::size_t count) {
  switch (encoding) {
    case VectorEncoding::Half: return count * 2;
    case VectorEncoding::Packed: return 1 + count * 3 + (count + 1) / 2;
    case VectorEncoding::ScaledByte: return 1 + count;
    case VectorEncoding::Full: return count * 4;
  }
  throw std::logic_error("unknown vector encoding");
}

inline int scaled_byte_exponent(std::uint32_t maximum) {
  return maximum == 0 ? 0 : static_cast<int>(maximum >> 23) - 133;
}

inline bool exact_scaled_byte(float value, float inverse, int &code) {
  const double scaled = static_cast<double>(value) * inverse;
  if (!(scaled >= -127.0 && scaled <= 127.0)) return false;
  code = static_cast<int>(scaled);
  return scaled == static_cast<double>(code);
}

inline bool put_scaled_bytes(Bytes &out, const float *values, std::size_t count, std::uint32_t maximum) {
  // Choose a power-of-two step that puts the largest magnitude in [64, 128).
  const int exponent = scaled_byte_exponent(maximum);
  if (exponent < -126 || exponent > 121) return false;
  const float inverse = float_from_bits(std::uint32_t(127 - exponent) << 23);
  const auto start = out.size();
  out.resize(start + vector_bytes(VectorEncoding::ScaledByte, count));
  out[start] = static_cast<std::uint8_t>(127 + exponent);
  for (std::size_t i = 0; i < count; ++i) {
    const auto bits = float_bits(values[i]);
    int code;
    if (!exact_scaled_byte(float_from_bits(bits & 0x7fffffffU), inverse, code)) {
      out.resize(start);
      return false;
    }
    out[start + 1 + i] = static_cast<std::uint8_t>((bits >> 24 & 0x80U) | code);
  }
  return true;
}

inline VectorEncoding put_vector(Bytes &out, const float *values, std::size_t count) {
  // Only encode bit patterns that round-trip exactly, including signed zero.
  // Nonzero values must be normal binary16 numbers with no discarded bits.
  bool half = true;
  bool scaled = count >= 16;
  std::uint32_t maximum = 0;
  unsigned min_exponent = 127;
  unsigned max_exponent = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const auto bits = float_bits(values[i]);
    const auto magnitude = bits & 0x7fffffffU;
    half &= magnitude == 0 || (magnitude >= 0x38800000U && magnitude <= 0x477fe000U &&
                               (magnitude & 0x1fffU) == 0);
    if (scaled && magnitude != 0) {
      // A normal number needs at most seven significant bits for this encoding.
      scaled = magnitude >= 0x00800000U && (magnitude & 0x1ffffU) == 0;
      if (magnitude > maximum) maximum = magnitude;
    }
    const auto exponent = magnitude >> 24;
    if (exponent < min_exponent) min_exponent = exponent;
    if (exponent > max_exponent) max_exponent = exponent;
    if (!half && !scaled && max_exponent - min_exponent > 7) break;
  }
  if (scaled && put_scaled_bytes(out, values, count, maximum)) return VectorEncoding::ScaledByte;
  if (half) {
    for (std::size_t i = 0; i < count; ++i) {
      const auto bits = float_bits(values[i]);
      const auto magnitude = bits & 0x7fffffffU;
      std::uint32_t code = (bits >> 16) & 0x8000U;
      if (magnitude != 0) code |= (magnitude - 0x38000000U) >> 13;
      out.push_back(static_cast<std::uint8_t>(code));
      out.push_back(static_cast<std::uint8_t>(code >> 8));
    }
    return VectorEncoding::Half;
  }
  if (count >= 16 && max_exponent - min_exponent <= 7) {
    // Pack sign and the upper exponent delta into a nibble per component.
    // The other 24 bits, including the entire mantissa, are stored verbatim.
    out.push_back(static_cast<std::uint8_t>(min_exponent));
    const auto code = [&](std::uint32_t bits) {
      return ((bits >> 28) & 8) | (((bits >> 24) & 127) - min_exponent);
    };
    const auto put_low_bytes = [&](std::uint32_t bits) {
      out.push_back(static_cast<std::uint8_t>(bits));
      out.push_back(static_cast<std::uint8_t>(bits >> 8));
      out.push_back(static_cast<std::uint8_t>(bits >> 16));
    };
    for (std::size_t i = 0; i < count; i += 2) {
      const auto first = float_bits(values[i]);
      const auto second = i + 1 < count ? float_bits(values[i + 1]) : 0;
      const auto second_code = i + 1 < count ? code(second) : 0;
      out.push_back(static_cast<std::uint8_t>(code(first) | (second_code << 4)));
      put_low_bytes(first);
      if (i + 1 < count) put_low_bytes(second);
    }
    return VectorEncoding::Packed;
  }
  put_floats(out, values, count);
  return VectorEncoding::Full;
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

  float f16() {
    const std::uint32_t low = byte();
    const std::uint32_t high = byte();
    const std::uint32_t half = low | (high << 8);
    const std::uint32_t magnitude = half & 0x7fffU;
    if (magnitude != 0 && (magnitude < 0x0400U || magnitude >= 0x7c00U)) {
      throw std::runtime_error("invalid lossless binary16 value in journal");
    }
    std::uint32_t bits = (half & 0x8000U) << 16;
    if (magnitude != 0) bits |= (magnitude << 13) + 0x38000000U;
    return float_from_bits(bits);
  }

  void vector(float *values, std::size_t count, VectorEncoding encoding) {
    if (encoding == VectorEncoding::Full) {
      floats(values, count);
    } else if (encoding == VectorEncoding::Half) {
      for (std::size_t i = 0; i < count; ++i) values[i] = f16();
    } else if (encoding == VectorEncoding::ScaledByte) {
      const std::uint32_t exponent = byte();
      if (exponent == 0 || exponent > 248) throw std::runtime_error("invalid scaled vector in journal");
      require(count);
      const auto *codes = input_.data() + position_;
      const auto scale = float_from_bits(exponent << 23);
      for (std::size_t i = 0; i < count; ++i) {
        const auto magnitude = static_cast<float>(codes[i] & 0x7fU) * scale;
        values[i] = (codes[i] & 0x80U) ? -magnitude : magnitude;
      }
      position_ += count;
    } else {
      const std::uint32_t base = byte();
      const auto component = [&](std::uint32_t code) {
        const auto exponent = base + (code & 7);
        if (exponent > 127) throw std::runtime_error("invalid packed exponent in journal");
        const std::uint32_t low = byte();
        const std::uint32_t middle = byte();
        const std::uint32_t high = byte();
        return float_from_bits(((code & 8) << 28) | (exponent << 24) | (high << 16) | (middle << 8) | low);
      };
      for (std::size_t i = 0; i < count; i += 2) {
        const auto pair = byte();
        values[i] = component(pair & 15);
        if (i + 1 < count) values[i + 1] = component(pair >> 4);
      }
    }
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
