// Portable independent brute-force oracle, extracted from benchmark_support.cpp.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <utility>
#include <vector>

template <typename T>
void read_values(T *values, std::size_t count) {
  std::cin.read(reinterpret_cast<char *>(values), count * sizeof(T));
  if (!std::cin) {
    throw std::runtime_error("incomplete reference input");
  }
}

void oracle() {
  std::array<std::uint32_t, 3> shape{};
  read_values(shape.data(), shape.size());
  const auto [rows, dimension, queries] = shape;
  if (rows == 0 || dimension == 0 || rows > 100000 || dimension > 4096 || queries > 10000) {
    throw std::runtime_error("invalid reference input shape");
  }
  std::vector<std::int32_t> buckets(rows);
  std::vector<float> vectors(static_cast<std::size_t>(rows) * dimension);
  std::vector<float> query(dimension);
  read_values(buckets.data(), buckets.size());
  read_values(vectors.data(), vectors.size());

  for (std::uint32_t q = 0; q < queries; ++q) {
    std::int32_t filter = -1;
    read_values(&filter, 1);
    read_values(query.data(), query.size());
    using Match = std::pair<double, std::uint32_t>;
    std::priority_queue<Match> best;
    for (std::uint32_t id = 0; id < rows; ++id) {
      if (filter >= 0 && buckets[id] != filter) continue;
      const std::size_t offset = static_cast<std::size_t>(id) * dimension;
      double distance = 0;
      for (std::size_t i = 0; i < dimension; ++i) {
        const double difference = double(vectors[offset + i]) - double(query[i]);
        distance += difference * difference;
      }
      const Match candidate{std::sqrt(distance), id};
      if (best.size() < 10) {
        best.push(candidate);
      } else if (candidate < best.top()) {
        best.pop();
        best.push(candidate);
      }
    }
    std::vector<std::uint32_t> order;
    while (!best.empty()) {
      order.push_back(best.top().second);
      best.pop();
    }
    for (auto id = order.rbegin(); id != order.rend(); ++id) std::cout << *id << ' ';
    std::cout << '\n';
  }
}


int main() { try { oracle(); } catch (const std::exception &e) { std::cerr << e.what(); return 1; } }
