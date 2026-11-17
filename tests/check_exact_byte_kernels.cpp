// Scalar oracle for full-precision signed and reversibly biased byte kernels.
#include "../src/database.cpp"
#include <iostream>
#include <random>

int main() {
#if defined(__x86_64__) && defined(__GNUC__)
  if (!__builtin_cpu_supports("avx2")) return 77;
  using namespace minivector;
  std::mt19937 rng(20261127);
  std::size_t checked = 0;
  for (const std::size_t dimension : {32U, 64U, 96U, 128U, 256U, 4096U}) {
    constexpr std::size_t count = 24;
    for (unsigned trial = 0; trial < 24; ++trial) {
      std::vector<std::int8_t> original(count * dimension), biased(original.size()), query(dimension);
      std::vector<std::int32_t> norms(count), distances(count);
      std::int32_t correction = 0;
      for (auto &code : query) {
        code = trial == 0 ? 127 : trial == 1 ? -127 : static_cast<int>(rng() % 255) - 127;
        correction += 128 * code;
      }
      for (std::size_t row = 0; row < count; ++row) {
        std::int32_t cross = 0;
        for (std::size_t i = 0; i < dimension; ++i) {
          const int value = trial < 2 ? (row % 2 ? -64 : 64) : static_cast<int>(rng() % 129) - 64;
          const auto offset = quantized_offset(row, i, dimension, count);
          original[offset] = value;
          biased[offset] = static_cast<std::int8_t>(value + 64);
          norms[row] += value * value;
          cross += value * query[i];
        }
        distances[row] = norms[row] - 2 * cross;
        const auto lane = row % quantized_block_rows;
        const auto *block = biased.data() + (row - lane) * dimension;
        if (quantized_block_row_dot<true>(block, lane, query.data(), dimension) - correction / 2 != cross) return 1;
        std::vector<std::int8_t> linear(dimension);
        for (std::size_t i = 0; i < dimension; ++i) linear[i] = biased[quantized_offset(row, i, dimension, count)];
        if (quantized_dot<true>(linear.data(), query.data(), dimension) - correction / 2 != cross) return 2;
      }
      for (const auto distance : distances) {
        for (const int delta : {-1, 0, 1}) {
          const auto cutoff = distance + delta;
          for (std::size_t begin = 0; begin < count; begin += quantized_block_rows) {
            std::array<std::int32_t, quantized_block_rows> got, baseline;
            unsigned mask = 0, baseline_mask = 0;
            const auto found = find_bounded_quantized_block(biased.data(), norms.data(), query.data(), dimension,
                cutoff, begin, count, got.data(), mask, correction);
            const auto old = find_quantized_block(original.data(), norms.data(), query.data(), dimension,
                cutoff, begin, count, baseline.data(), baseline_mask);
            std::size_t expected = begin;
            unsigned expected_mask = 0;
            for (; expected < count; expected += quantized_block_rows) {
              for (unsigned lane = 0; lane < quantized_block_rows; ++lane)
                if (distances[expected + lane] <= cutoff) expected_mask |= 1U << lane;
              if (expected_mask) break;
            }
            if (found != expected || mask != expected_mask || old != expected || baseline_mask != expected_mask) return 3;
            if (mask) for (unsigned lane = 0; lane < quantized_block_rows; ++lane)
              if (got[lane] != distances[found + lane] || baseline[lane] != got[lane]) return 4;
            if (__builtin_cpu_supports("ssse3")) {
              unsigned fallback_mask = 0;
              const auto fallback = find_quantized_block_ssse3(original.data(), norms.data(), query.data(), dimension,
                  cutoff, begin, count, baseline.data(), fallback_mask);
              if (fallback != expected || fallback_mask != expected_mask) return 5;
              if (fallback_mask) for (unsigned lane = 0; lane < quantized_block_rows; ++lane)
                if (baseline[lane] != distances[fallback + lane]) return 6;
            }
            ++checked;
          }
        }
      }
    }
  }
  std::cout << "PASS scalar checks: " << checked << " block cutoffs; saturation extremes and distance ties included\n";
#else
  return 77;
#endif
}
