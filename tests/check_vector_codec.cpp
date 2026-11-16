#include "../src/encoding.hpp"

#include <algorithm>
#include <iostream>
#include <random>
#include <sys/mman.h>
#include <unistd.h>

using namespace minivector;

int main() {
  std::mt19937 random(20261117);
  const auto page = static_cast<std::size_t>(::sysconf(_SC_PAGESIZE));
  auto *region = static_cast<std::uint8_t *>(::mmap(nullptr, 8 * page, PROT_READ | PROT_WRITE,
                                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (region == MAP_FAILED || ::mprotect(region + 7 * page, page, PROT_NONE) != 0) return 1;
  std::size_t checks = 0;
  for (const std::size_t dimension : {16, 17, 18, 19, 31, 32, 33, 127, 128, 129, 4095}) {
    for (unsigned turn = 0; turn < 24; ++turn) {
      const unsigned base = turn % 3 == 0 ? 0 : turn % 3 == 1 ? 120 : 124;
      Bytes packed{static_cast<std::uint8_t>(base)};
      std::vector<float> expected(dimension), actual(dimension);
      for (std::size_t i = 0; i < dimension; i += 2) {
        std::uint32_t bits[2]{};
        unsigned codes[2]{};
        for (unsigned lane = 0; lane < 2 && i + lane < dimension; ++lane) {
          const auto exponent = base + random() % std::min(8U, 128 - base);
          bits[lane] = (random() & 0x80ffffffU) | (exponent << 24);
          if (base == 0 && i + lane < 4) {
            const std::uint32_t edge[] = {0, 0x80000000U, 1, 0x80000001U};
            bits[lane] = edge[i + lane];
          }
          codes[lane] = ((bits[lane] >> 28) & 8) | ((bits[lane] >> 24 & 127) - base);
          expected[i + lane] = float_from_bits(bits[lane]);
        }
        packed.push_back(static_cast<std::uint8_t>(codes[0] | (codes[1] << 4)));
        for (unsigned lane = 0; lane < 2 && i + lane < dimension; ++lane)
          for (unsigned shift = 0; shift < 24; shift += 8)
            packed.push_back(static_cast<std::uint8_t>(bits[lane] >> shift));
      }
      // End exactly at an inaccessible page, with all four SIMD tail lengths.
      auto *input = region + 7 * page - packed.size();
      std::copy(packed.begin(), packed.end(), input);
      Reader reader(input, packed.size());
      reader.vector(actual.data(), dimension, VectorEncoding::Packed);
      reader.finish();
      for (std::size_t i = 0; i < dimension; ++i) {
        if (float_bits(expected[i]) != float_bits(actual[i])) return 2;
      }
      // Truncation and malformed exponents must retain the checked decoder's
      // rejection, including when corruption occurs in a SIMD tail.
      try {
        Reader truncated(input, packed.size() - 1);
        truncated.vector(actual.data(), dimension, VectorEncoding::Packed);
        return 3;
      } catch (const std::runtime_error &) {}
      for (const auto pair : {std::size_t{0}, (dimension - 1) / 2}) {
        std::copy(packed.begin(), packed.end(), input);
        input[0] = 127;
        input[1 + pair * 7] |= 7;
        try {
          Reader invalid(input, packed.size());
          invalid.vector(actual.data(), dimension, VectorEncoding::Packed);
          return 4;
        } catch (const std::runtime_error &) {}
      }
      ++checks;
    }
  }
  ::munmap(region, 8 * page);
  std::cout << checks << " packed vector bit-pattern, guard-page, truncation and corrupt-exponent checks: PASS\n";
}
