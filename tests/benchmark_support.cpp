// Independent reference search and Linux container counter reader for local tests.
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <utility>
#include <vector>

namespace {

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
      const Match candidate{distance, id};
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

std::string counter(const char *path) {
  std::ifstream file(path);
  std::string value;
  if (!std::getline(file, value)) {
    throw std::runtime_error(std::string("missing cgroup counter ") + path);
  }
  return value;
}

void probe() {
  struct stat data{};
  struct stat journal{};
  if (::stat("/data", &data) != 0 || ::stat("/data/store.wal", &journal) != 0) {
    throw std::runtime_error("cannot identify the benchmark data device");
  }
  std::ifstream io("/sys/fs/cgroup/io.stat");
  if (!io) {
    throw std::runtime_error("cgroup v2 io.stat is unavailable");
  }
  std::cout << major(data.st_dev) << ':' << minor(data.st_dev) << '\n'
            << counter("/sys/fs/cgroup/memory.peak") << '\n'
            << counter("/sys/fs/cgroup/memory.max") << '\n'
            << counter("/sys/fs/cgroup/memory.swap.max") << '\n'
            << journal.st_size << '\n'
            << io.rdbuf();
}

}  // namespace

int main(int argc, char **argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  try {
    if (argc == 2 && std::string_view(argv[1]) == "oracle") {
      oracle();
    } else if (argc == 2 && std::string_view(argv[1]) == "probe") {
      probe();
    } else {
      throw std::invalid_argument("expected oracle or probe");
    }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
