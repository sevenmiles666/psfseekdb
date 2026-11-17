#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

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

inline std::uint64_t signed_code(std::int64_t value) {
  return (static_cast<std::uint64_t>(value) << 1) ^ (value < 0 ? UINT64_MAX : 0);
}

inline void put_varint(Bytes &out, std::uint64_t code) {
  while (code >= 128) {
    out.push_back(static_cast<std::uint8_t>(code | 128U));
    code >>= 7;
  }
  out.push_back(static_cast<std::uint8_t>(code));
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

enum class VectorEncoding { Full, Half, Packed, ScaledByte, ScaledBits, ScaledRange, FixedDecimal, SharedExponent };

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
    case VectorEncoding::ScaledBits: return 2 + (count + 7) / 8;  // Minimum before reading its width.
    case VectorEncoding::ScaledRange: return 3 + (count + 7) / 8;
    case VectorEncoding::FixedDecimal: return 1 + (count + 7) / 8;
    case VectorEncoding::SharedExponent:
      return count <= 1 ? count * 4 : 5 + 3 * (count - 1) + (count + 2) / 4;
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

inline bool put_scaled_bytes(Bytes &out, const float *values, std::size_t count, std::uint32_t maximum,
                             VectorEncoding &encoding) {
  // Choose a power-of-two step that puts the largest magnitude in [64, 128).
  const int exponent = scaled_byte_exponent(maximum);
  if (exponent < -126 || exponent > 121) return false;
  const float inverse = float_from_bits(std::uint32_t(127 - exponent) << 23);
  const auto start = out.size();
  out.resize(start + vector_bytes(VectorEncoding::ScaledByte, count));
  out[start] = static_cast<std::uint8_t>(127 + exponent);
  unsigned combined = 0;
  int minimum = 127;
  int maximum_code = -127;
  for (std::size_t i = 0; i < count; ++i) {
    const auto bits = float_bits(values[i]);
    int code;
    if (!exact_scaled_byte(float_from_bits(bits & 0x7fffffffU), inverse, code)) {
      out.resize(start);
      return false;
    }
    combined |= static_cast<unsigned>(code);
    out[start + 1 + i] = static_cast<std::uint8_t>((bits >> 24 & 0x80U) | code);
    const int signed_value = bits >> 31 ? -code : code;
    if (signed_value < minimum) minimum = signed_value;
    if (signed_value > maximum_code) maximum_code = signed_value;
  }
  // Factor out trailing zero bits shared by every magnitude. Keep one sign
  // bit even for a zero component, preserving the original FP32 bit pattern.
  unsigned shift = 0;
  if (combined != 0) {
    while ((combined & 1U) == 0) {
      combined >>= 1;
      ++shift;
    }
  }
  unsigned width = 1;
  for (; combined != 0; combined >>= 1) ++width;
  const auto packed_size = (count * width + 7) / 8;
  // Short codes cover most of a narrow signed range. Code zero also
  // preserves negative zero, independently of the minimum stored value.
  const int divisor = 1 << shift;
  minimum /= divisor;
  maximum_code /= divisor;
  const auto radix = static_cast<unsigned>(maximum_code - minimum + 2);
  unsigned short_width = 0;
  for (auto symbols = radix; symbols > 1; symbols >>= 1) ++short_width;
  const auto short_codes = (2U << short_width) - radix;
  const auto symbol_at = [&](std::size_t i) {
    const auto code = out[start + 1 + i];
    const auto magnitude = static_cast<int>((code & 0x7fU) >> shift);
    if (code == 0x80U) return 0U;
    return static_cast<unsigned>((code & 0x80U ? -magnitude : magnitude) - minimum + 1);
  };
  const auto previous_size = std::min(1 + count, 2 + packed_size);
  if (3 + (count * short_width + 7) / 8 < previous_size) {
    auto total_bits = count * short_width;
    for (std::size_t i = 0; i < count; ++i) total_bits += symbol_at(i) >= short_codes;
    if (3 + (total_bits + 7) / 8 < previous_size) {
      unsigned pending = 0;
      unsigned bits = 0;
      // Compact over bytes already consumed, then make room for the header.
      auto destination = start;
      for (std::size_t i = 0; i < count; ++i) {
        const auto symbol = symbol_at(i);
        if (symbol < short_codes) {
          pending |= symbol << bits;
          bits += short_width;
        } else {
          const auto extended = symbol + short_codes;
          const auto code = (extended >> 1) | ((extended & 1U) << short_width);
          pending |= code << bits;
          bits += short_width + 1;
        }
        if (bits >= 8) {
          out[destination++] = static_cast<std::uint8_t>(pending);
          pending >>= 8;
          bits -= 8;
        }
      }
      if (bits != 0) out[destination++] = static_cast<std::uint8_t>(pending);
      const auto length = destination - start;
      std::memmove(out.data() + start + 3, out.data() + start, length);
      out[start] = static_cast<std::uint8_t>(127 + exponent + shift);
      out[start + 1] = static_cast<std::uint8_t>(minimum + 127);
      out[start + 2] = static_cast<std::uint8_t>(radix - 1);
      out.resize(start + 3 + length);
      encoding = VectorEncoding::ScaledRange;
      return true;
    }
  }
  encoding = VectorEncoding::ScaledByte;
  if (1 + packed_size >= count) return true;

  unsigned pending = 0;
  unsigned bits = 0;
  auto destination = start + 2;
  for (std::size_t i = 0; i < count; ++i) {
    const auto code = out[start + 1 + i];
    const auto packed = ((code >> 7) << (width - 1)) | ((code & 0x7fU) >> shift);
    pending |= packed << bits;
    bits += width;
    if (bits >= 8) {
      out[destination++] = static_cast<std::uint8_t>(pending);
      pending >>= 8;
      bits -= 8;
    }
  }
  if (bits != 0) out[destination++] = static_cast<std::uint8_t>(pending);
  out[start] = static_cast<std::uint8_t>(127 + exponent + shift);
  out[start + 1] = static_cast<std::uint8_t>(width);
  out.resize(destination);
  encoding = VectorEncoding::ScaledBits;
  return true;
}

constexpr std::array<double, 7> decimal_scales{10.0, 100.0, 1000.0, 10000.0, 100000.0, 1000000.0, 10000000.0};

inline bool put_fixed_decimals(Bytes &out, const float *values, std::size_t count, std::size_t previous_size) {
  if (count < 16) return false;
  constexpr std::uint32_t magnitude_limit = 1U << 26;
  const auto start = out.size();
  for (unsigned scale = 0; scale < decimal_scales.size(); ++scale) {
    const auto factor = decimal_scales[scale];
    const auto step = 1.0 / factor;
    const auto code = [&](float value, std::uint32_t &word) {
      const auto bits = float_bits(value);
      const auto magnitude = bits & 0x7fffffffU;
      const double scaled = static_cast<double>(float_from_bits(magnitude)) * factor + 0.5;
      if (!(scaled < magnitude_limit)) return false;
      const auto integer = static_cast<std::uint32_t>(scaled);
      if (float_bits(static_cast<float>(integer * step)) != magnitude) return false;
      word = integer | (bits & 0x80000000U);
      return true;
    };
    bool exact = true;
    for (std::size_t i = 0; i < 6; ++i) {
      std::uint32_t word;
      if (!code(values[i * count / 6], word)) { exact = false; break; }
    }
    if (!exact) continue;

    out.resize(start + count * sizeof(std::uint32_t));
    auto *const staging = out.data() + start;
    std::uint32_t combined = 0;
    for (std::size_t i = 0; i < count; ++i) {
      std::uint32_t word;
      if (!code(values[i], word)) { exact = false; break; }
      combined |= word & 0x7fffffffU;
      std::memcpy(staging + i * sizeof(word), &word, sizeof(word));
    }
    if (!exact) { out.resize(start); continue; }
    unsigned width = 1;
    for (; combined != 0; combined >>= 1) ++width;
    if (1 + (count * width + 7) / 8 >= previous_size) { out.resize(start); return false; }

    std::uint64_t pending = 0;
    unsigned bits = 0;
    auto *destination = staging;
    for (std::size_t i = 0; i < count; ++i) {
      std::uint32_t word;
      std::memcpy(&word, staging + i * sizeof(word), sizeof(word));
      const auto packed = (word & 0x7fffffffU) | ((word >> 31) << (width - 1));
      pending |= static_cast<std::uint64_t>(packed) << bits;
      bits += width;
      while (bits >= 8) {
        *destination++ = static_cast<std::uint8_t>(pending);
        pending >>= 8;
        bits -= 8;
      }
    }
    if (bits != 0) *destination++ = static_cast<std::uint8_t>(pending);
    const auto bytes = static_cast<std::size_t>(destination - staging);
    std::memmove(staging + 1, staging, bytes);
    out[start] = static_cast<std::uint8_t>(scale << 5 | width);
    out.resize(start + 1 + bytes);
    return true;
  }
  return false;
}

inline bool put_shared_exponent(Bytes &out, const float *values, std::size_t count,
                                std::size_t previous_size) {
  if (count < 64) return false;
  unsigned base = 0;
  for (std::size_t i = 1; i < count; ++i) {
    base = std::max(base, (float_bits(values[i]) >> 23) & 255U);
  }
  std::size_t escapes = 0;
  for (std::size_t i = 1; i < count; ++i) {
    escapes += base - ((float_bits(values[i]) >> 23) & 255U) >= 3;
  }
  const auto bytes = vector_bytes(VectorEncoding::SharedExponent, count) + escapes;
  if (bytes >= previous_size) return false;

  // Keep the first coordinate directly readable when replay only needs the projection.
  put_float(out, values[0]);
  const auto start = out.size();
  out.resize(start + bytes - sizeof(float));
  auto *destination = out.data() + start;
  *destination++ = static_cast<std::uint8_t>(base);
  for (std::size_t group = 1; group < count; group += 4) {
    auto *tags = destination++;
    unsigned codes = 0;
    for (std::size_t i = group; i < std::min(count, group + 4); ++i) {
      const auto bits = float_bits(values[i]);
      const auto exponent = (bits >> 23) & 255U;
      const auto delta = base - exponent;
      codes |= std::min(delta, 3U) << (2 * (i - group));
      const auto stored = (bits & 0x7fffffU) | ((bits >> 8) & 0x800000U);
      *destination++ = static_cast<std::uint8_t>(stored);
      *destination++ = static_cast<std::uint8_t>(stored >> 8);
      *destination++ = static_cast<std::uint8_t>(stored >> 16);
      if (delta >= 3) *destination++ = static_cast<std::uint8_t>(exponent);
    }
    *tags = static_cast<std::uint8_t>(codes);
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
  VectorEncoding scaled_encoding;
  if (scaled && put_scaled_bytes(out, values, count, maximum, scaled_encoding)) return scaled_encoding;
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
  const bool packed = count >= 16 && max_exponent - min_exponent <= 7;
  const auto previous_encoding = packed ? VectorEncoding::Packed : VectorEncoding::Full;
  if (put_fixed_decimals(out, values, count, vector_bytes(previous_encoding, count))) {
    return VectorEncoding::FixedDecimal;
  }
  if (put_shared_exponent(out, values, count, vector_bytes(previous_encoding, count))) {
    return VectorEncoding::SharedExponent;
  }
  if (packed) {
    // Pack sign and the upper exponent delta into a nibble per component.
    // The other 24 bits, including the entire mantissa, are stored verbatim.
    const auto start = out.size();
    out.resize(start + vector_bytes(VectorEncoding::Packed, count));
    auto *destination = out.data() + start;
    *destination++ = static_cast<std::uint8_t>(min_exponent);
    const auto code = [&](std::uint32_t bits) {
      return ((bits >> 28) & 8) | (((bits >> 24) & 127) - min_exponent);
    };
    const auto put_low_bytes = [&](std::uint32_t bits) {
      *destination++ = static_cast<std::uint8_t>(bits);
      *destination++ = static_cast<std::uint8_t>(bits >> 8);
      *destination++ = static_cast<std::uint8_t>(bits >> 16);
    };
    for (std::size_t i = 0; i < count; i += 2) {
      const auto first = float_bits(values[i]);
      const auto second = i + 1 < count ? float_bits(values[i + 1]) : 0;
      const auto second_code = i + 1 < count ? code(second) : 0;
      *destination++ = static_cast<std::uint8_t>(code(first) | (second_code << 4));
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

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2")))
inline std::size_t decode_packed_vectors(const std::uint8_t *source, float *values,
                                        std::size_t count, std::uint32_t base) {
  const auto mantissa = _mm256_broadcastsi128_si256(_mm_setr_epi8(
      1, 2, 3, -1, 4, 5, 6, -1, 8, 9, 10, -1, 11, 12, 13, -1));
  const auto codes = _mm256_broadcastsi128_si256(_mm_setr_epi8(
      -1, -1, -1, 0, -1, -1, -1, 0, -1, -1, -1, 7, -1, -1, -1, 7));
  const auto high_bytes = _mm256_broadcastsi128_si256(_mm_setr_epi8(
      0, 1, 2, 3, 4, 5, 6, 7, -128, -127, -126, -125, -124, -123, -122, -121));
  const auto bases = _mm256_set1_epi32(static_cast<int>(base << 24));
  const auto nibble_mask = _mm256_set1_epi32(0x0f000000);
  std::size_t i = 0;
  // Each lane loads 14 encoded bytes plus two bytes from the following group.
  for (; i + 8 < count; i += 8, source += 28) {
    const auto first = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
    const auto second = _mm_loadu_si128(reinterpret_cast<const __m128i *>(source + 14));
    const auto octet = _mm256_set_m128i(second, first);
    const auto pairs = _mm256_shuffle_epi8(octet, codes);
    const auto nibble = _mm256_and_si256(
        _mm256_blend_epi16(pairs, _mm256_srli_epi32(pairs, 4), 0x88), nibble_mask);
    const auto exponent = _mm256_add_epi8(_mm256_shuffle_epi8(high_bytes, nibble), bases);
    const auto low = _mm256_shuffle_epi8(octet, mantissa);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(values + i), _mm256_or_si256(exponent, low));
  }
  return i;
}
#endif

class Reader {
public:
  explicit Reader(const Bytes &input) : Reader(input.data(), input.size()) {}
  Reader(const std::uint8_t *input, std::size_t length) : input_(input), length_(length) {}

  std::size_t remaining() const { return length_ - position_; }

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

  std::uint64_t varint() {
    std::uint64_t value = 0;
    for (unsigned shift = 0; shift < 64; shift += 7) {
      const std::uint64_t next = byte();
      if (shift == 63 && next > 1) break;
      value |= (next & 127) << shift;
      if (next < 128) return value;
    }
    throw std::runtime_error("invalid variable-length integer in journal");
  }

  std::int64_t signed_varint() {
    const auto code = varint();
    const auto bits = (code >> 1) ^ (std::uint64_t{0} - (code & 1));
    std::int64_t value;
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
    } else if (encoding == VectorEncoding::SharedExponent) {
      if (count == 0) return;
      values[0] = f32();
      if (count == 1) return;
      const unsigned base = byte();
      if (base == 255) throw std::runtime_error("invalid shared exponent in journal");
      for (std::size_t group = 1; group < count; group += 4) {
        const unsigned tags = byte();
        for (std::size_t i = group; i < std::min(count, group + 4); ++i) {
          require(3);
          const auto *stored = input_ + position_;
          const std::uint32_t bits = stored[0] | (std::uint32_t(stored[1]) << 8) |
                                     (std::uint32_t(stored[2]) << 16);
          position_ += 3;
          const auto delta = (tags >> (2 * (i - group))) & 3U;
          const auto exponent = delta == 3 ? byte() : base - delta;
          if (exponent >= 255) throw std::runtime_error("invalid component exponent in journal");
          values[i] = float_from_bits(((bits & 0x800000U) << 8) | (exponent << 23) | (bits & 0x7fffffU));
        }
      }
    } else if (encoding == VectorEncoding::Half) {
      for (std::size_t i = 0; i < count; ++i) values[i] = f16();
    } else if (encoding == VectorEncoding::ScaledByte) {
      const std::uint32_t exponent = byte();
      if (exponent == 0 || exponent > 248) throw std::runtime_error("invalid scaled vector in journal");
      require(count);
      const auto *codes = input_ + position_;
      const auto scale = float_from_bits(exponent << 23);
      for (std::size_t i = 0; i < count; ++i) {
        const auto magnitude = static_cast<float>(codes[i] & 0x7fU) * scale;
        values[i] = (codes[i] & 0x80U) ? -magnitude : magnitude;
      }
      position_ += count;
    } else if (encoding == VectorEncoding::ScaledBits) {
      const std::uint32_t exponent = byte();
      const unsigned width = byte();
      if (exponent == 0 || exponent == 255 || width == 0 || width > 7) {
        throw std::runtime_error("invalid bit-packed vector in journal");
      }
      require((count * width + 7) / 8);
      const auto scale = float_from_bits(exponent << 23);
      const unsigned sign = 1U << (width - 1);
      unsigned pending = 0;
      unsigned bits = 0;
      for (std::size_t i = 0; i < count; ++i) {
        if (bits < width) {
          pending |= static_cast<unsigned>(input_[position_++]) << bits;
          bits += 8;
        }
        const auto magnitude = static_cast<float>(pending & (sign - 1)) * scale;
        values[i] = (pending & sign) ? -magnitude : magnitude;
        pending >>= width;
        bits -= width;
      }
    } else if (encoding == VectorEncoding::ScaledRange) {
      const std::uint32_t exponent = byte();
      const auto minimum = static_cast<int>(byte()) - 127;
      const unsigned radix = static_cast<unsigned>(byte()) + 1;
      if (exponent == 0 || exponent == 255 || radix < 2 ||
          minimum + static_cast<int>(radix) - 2 > 127) {
        throw std::runtime_error("invalid range-packed vector in journal");
      }
      unsigned width = 0;
      for (auto symbols = radix; symbols > 1; symbols >>= 1) ++width;
      const auto short_codes = (2U << width) - radix;
      const auto scale = float_from_bits(exponent << 23);
      const auto mask = (1U << width) - 1;
      unsigned pending = 0;
      unsigned bits = 0;
      const auto refill = [&] {
        pending |= static_cast<unsigned>(byte()) << bits;
        bits += 8;
      };
      for (std::size_t i = 0; i < count; ++i) {
        if (bits < width) refill();
        auto symbol = pending & mask;
        pending >>= width;
        bits -= width;
        if (symbol >= short_codes) {
          if (bits == 0) refill();
          symbol = (symbol << 1 | (pending & 1U)) - short_codes;
          pending >>= 1;
          --bits;
        }
        values[i] = symbol == 0 ? -0.0f : (minimum + static_cast<int>(symbol) - 1) * scale;
      }
    } else if (encoding == VectorEncoding::FixedDecimal) {
      const auto encoding_byte = byte();
      const unsigned scale = encoding_byte >> 5;
      const unsigned width = encoding_byte & 31;
      if (scale >= decimal_scales.size() || width == 0 || width > 27) {
        throw std::runtime_error("invalid fixed-point vector in journal");
      }
      const auto length = (count * width + 7) / 8;
      require(length);
      const auto step = 1.0 / decimal_scales[scale];
      const auto sign = 1U << (width - 1);
      const auto *source = input_ + position_;
      std::uint64_t pending = 0;
      unsigned bits = 0;
      for (std::size_t i = 0; i < count; ++i) {
        while (bits < width) {
          pending |= static_cast<std::uint64_t>(*source++) << bits;
          bits += 8;
        }
        const auto value = static_cast<float>((pending & (sign - 1)) * step);
        values[i] = pending & sign ? -value : value;
        pending >>= width;
        bits -= width;
      }
      position_ += length;
    } else {
      const std::uint32_t base = byte();
      const auto packed_bytes = count * 3 + (count + 1) / 2;
      require(packed_bytes);
      const auto *source = input_ + position_;
      const bool validate_exponent = base > 120;
      std::size_t decoded = 0;
#if defined(__x86_64__) && defined(__GNUC__)
      if (count >= 32 && !validate_exponent && __builtin_cpu_supports("avx2")) {
        decoded = decode_packed_vectors(source, values, count, base);
        source += decoded * 7 / 2;
      }
#endif
      const auto component = [&](std::uint32_t code) {
        const auto exponent = base + (code & 7);
        if (validate_exponent && exponent > 127) throw std::runtime_error("invalid packed exponent in journal");
        const std::uint32_t low = *source++;
        const std::uint32_t middle = *source++;
        const std::uint32_t high = *source++;
        return float_from_bits(((code & 8) << 28) | (exponent << 24) | (high << 16) | (middle << 8) | low);
      };
      for (std::size_t i = decoded; i < count; i += 2) {
        const auto pair = *source++;
        values[i] = component(pair & 15);
        if (i + 1 < count) values[i + 1] = component(pair >> 4);
      }
      position_ += packed_bytes;
    }
  }

  void floats(float *values, std::size_t count) {
    if (count > remaining() / sizeof(float)) {
      throw std::runtime_error("incomplete vector in journal");
    }
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    const auto length = count * sizeof(float);
    if (length != 0) std::memcpy(values, input_ + position_, length);
    position_ += length;
#else
    for (std::size_t i = 0; i < count; ++i) values[i] = f32();
#endif
  }

  std::string string() {
    const std::size_t length = u32();
    require(length);
    const auto *start = reinterpret_cast<const char *>(input_ + position_);
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

  const std::uint8_t *input_;
  std::size_t length_;
  std::size_t position_ = 0;
};

}  // namespace minivector
