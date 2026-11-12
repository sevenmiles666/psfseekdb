#pragma once
#include "json.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mvdb {
using Json = nlohmann::json;
struct Error : std::runtime_error {
  std::string code;
  Error(std::string c, std::string m) : std::runtime_error(std::move(m)), code(std::move(c)) {}
};
inline void require(bool ok, const std::string& code, const std::string& message) {
  if (!ok) throw Error(code, message);
}
inline void io_require(bool ok) {
  if (!ok) throw Error("io_error", std::strerror(errno));
}
inline uint64_t checksum(const void* data, size_t n) {
  auto* p = static_cast<const unsigned char*>(data);
  uint64_t h = 14695981039346656037ULL;
  while (n--) { h ^= *p++; h *= 1099511628211ULL; }
  return h;
}
inline void fault(const char* point) {
  // Development fault injection is opt-in, silent, and never changes successful results.
  const char* selected = std::getenv("MVDB_CRASH_AT");
  if (selected && std::string(selected) == point) _exit(86);
}
inline uint64_t read64(const unsigned char* p) {
  uint64_t x = 0;
  for (unsigned i = 0; i < 8; ++i) x |= uint64_t(p[i]) << (i * 8);
  return x;
}
inline void write64(unsigned char* p, uint64_t x) {
  for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<unsigned char>(x >> (i * 8));
}

// Immutable records and B+ tree pages share a single append-only file. Only the
// alternating root descriptors are overwritten; no committed page is updated.
class File {
  int fd_ = -1;
  int dir_ = -1;
  uint64_t end_ = 8192, generation_ = 0;
  std::vector<unsigned char> pending_;
  uint64_t pending_start_ = 8192;
  std::string directory_;
  void write_at(uint64_t offset, const void* data, size_t n) {
    auto* p = static_cast<const unsigned char*>(data);
    while (n) {
      ssize_t k = pwrite(fd_, p, n, static_cast<off_t>(offset));
      if (k < 0 && errno == EINTR) continue;
      io_require(k > 0); p += k; n -= size_t(k); offset += uint64_t(k);
    }
  }
 public:
  Json catalog = Json::object();
  explicit File(const std::string& directory) : directory_(directory) {
    std::filesystem::create_directories(directory);
    dir_ = open(directory.c_str(), O_RDONLY | O_DIRECTORY); io_require(dir_ >= 0);
    fd_ = open((directory + "/database.mvdb").c_str(), O_RDWR | O_CREAT, 0600);
    io_require(fd_ >= 0);
    io_require(flock(fd_, LOCK_EX | LOCK_NB) == 0);
    struct stat st{}; io_require(fstat(fd_, &st) == 0);
    if (st.st_size == 0) {
      io_require(ftruncate(fd_, 8192) == 0);
      io_require(fsync(fd_) == 0); io_require(fsync(dir_) == 0);
      end_ = 8192;
    } else {
      require(st.st_size >= 8192, "corrupt_storage", "Truncated root area");
      end_ = uint64_t(st.st_size);
      pending_start_ = end_;
      uint64_t committed_end = 8192;
      bool had_nonzero = false;
      for (unsigned slot = 0; slot < 2; ++slot) {
        auto b = read(slot * 4096, 64);
        if (std::any_of(b.begin(), b.end(), [](auto c){ return c != 0; })) had_nonzero = true;
        if (read64(b.data()) != 0x4D564442524F4F54ULL ||
            checksum(b.data(), 48) != read64(b.data() + 48)) continue;
        auto gen = read64(b.data() + 8), pos = read64(b.data() + 16);
        auto len = read64(b.data() + 24), sum = read64(b.data() + 32);
        if (gen <= generation_ || pos < 8192 || len > 64 * 1024 * 1024 ||
            pos > end_ || len > end_ - pos) continue;
        auto metadata = read(pos, size_t(len));
        if (checksum(metadata.data(), metadata.size()) != sum) continue;
        try {
          auto candidate = Json::from_cbor(metadata);
          if (!candidate.is_object()) continue;
          catalog = std::move(candidate); generation_ = gen;
          committed_end = pos + len;
        } catch (const Json::exception&) { continue; }
      }
      require(!had_nonzero || generation_ != 0, "corrupt_storage", "No valid committed root");
      // Anything after the published catalog belongs to an uncommitted append.
      io_require(ftruncate(fd_, off_t(committed_end)) == 0);
      end_ = committed_end;
    }
    pending_start_ = end_;
    fault("recovery_loaded");
  }
  ~File() { if (fd_ >= 0) close(fd_); if (dir_ >= 0) close(dir_); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  uint64_t position() const { return end_; }
  uint64_t generation() const { return generation_; }
  const std::string& directory() const { return directory_; }
  void replace(File& compacted) {
    fault("before_compaction_rename");
    io_require(rename((compacted.directory_ + "/database.mvdb").c_str(),
                      (directory_ + "/database.mvdb").c_str()) == 0);
    fault("after_compaction_rename"); io_require(fsync(dir_) == 0);
    std::swap(fd_, compacted.fd_); std::swap(end_, compacted.end_);
    std::swap(generation_, compacted.generation_); std::swap(pending_start_, compacted.pending_start_);
    pending_.swap(compacted.pending_); catalog.swap(compacted.catalog);
  }
  std::vector<unsigned char> read(uint64_t pos, size_t n) {
    require(pos <= end_ && n <= end_ - pos, "corrupt_storage", "Read outside file");
    std::vector<unsigned char> out(n);
    size_t done = 0;
    if (pos < pending_start_) {
      size_t disk_n = size_t(std::min<uint64_t>(n, pending_start_ - pos));
      while (done < disk_n) {
        ssize_t k = pread(fd_, out.data() + done, disk_n - done, off_t(pos + done));
        if (k < 0 && errno == EINTR) continue;
        io_require(k > 0); done += size_t(k);
      }
    }
    if (done < n) {
      auto offset = pos + done - pending_start_;
      require(offset + n - done <= pending_.size(), "corrupt_storage", "Invalid append buffer");
      std::memcpy(out.data() + done, pending_.data() + offset, n - done);
    }
    return out;
  }
  uint64_t append(const void* data, size_t n) {
    uint64_t pos = end_;
    auto* p = static_cast<const unsigned char*>(data);
    while (n) {
      size_t k = std::min(n, size_t(1024 * 1024) - pending_.size());
      pending_.insert(pending_.end(), p, p + k); p += k; n -= k; end_ += k;
      if (pending_.size() == 1024 * 1024) flush();
    }
    return pos;
  }
  void flush() {
    if (!pending_.empty()) write_at(pending_start_, pending_.data(), pending_.size());
    pending_.clear(); pending_start_ = end_;
  }
  void truncate(uint64_t position) {
    require(position <= end_, "internal_error", "Invalid rollback offset");
    if (position >= pending_start_) pending_.resize(size_t(position - pending_start_));
    else { pending_.clear(); io_require(ftruncate(fd_, off_t(position)) == 0); pending_start_ = position; }
    end_ = position;
  }
  void publish(const Json& state) {
    auto metadata = Json::to_cbor(state);
    auto pos = append(metadata.data(), metadata.size());
    flush(); fault("before_data_sync");
    io_require(fsync(fd_) == 0); fault("after_data_sync");
    std::array<unsigned char, 64> root{};
    write64(root.data(), 0x4D564442524F4F54ULL);
    write64(root.data() + 8, generation_ + 1);
    write64(root.data() + 16, pos); write64(root.data() + 24, metadata.size());
    write64(root.data() + 32, checksum(metadata.data(), metadata.size()));
    write64(root.data() + 40, 1);
    write64(root.data() + 48, checksum(root.data(), 48));
    write_at(((generation_ + 1) % 2) * 4096, root.data(), root.size());
    fault("after_root_write"); io_require(fsync(fd_) == 0); fault("after_root_sync");
    ++generation_; catalog = state;
  }
};

struct Entry { int64_t key = 0; uint64_t position = 0, length = 0; };
struct Node { bool leaf = true; std::vector<Entry> entries; };
class Tree {
  static constexpr size_t capacity = 128;
  static constexpr uint64_t temporary = uint64_t(1) << 63;
  File& file_;
  uint64_t sequence_ = temporary;
  std::unordered_map<uint64_t, Node> dirty_;
  std::unordered_map<uint64_t, Node> cache_;
  Node load(uint64_t pos) {
    if (pos >= temporary) return dirty_.at(pos);
    if (auto it = cache_.find(pos); it != cache_.end()) return it->second;
    auto b = file_.read(pos, 4096);
    require(read64(b.data()) == 0x4D56444250414745ULL &&
            checksum(b.data(), 4088) == read64(b.data() + 4088), "corrupt_storage", "Invalid tree page");
    size_t n = size_t(read64(b.data() + 16));
    require(n > 0 && n <= capacity, "corrupt_storage", "Invalid tree fanout");
    Node node; node.leaf = read64(b.data() + 8) != 0;
    for (size_t i = 0; i < n; ++i) {
      auto* p = b.data() + 24 + i * 24;
      node.entries.push_back({int64_t(read64(p)), read64(p + 8), read64(p + 16)});
    }
    if (cache_.size() >= 2048) cache_.clear();
    cache_.emplace(pos, node); return node;
  }
  uint64_t stage(uint64_t old, Node node) {
    auto id = old >= temporary ? old : ++sequence_;
    dirty_[id] = std::move(node); return id;
  }
  std::vector<Entry> change(uint64_t root, int64_t key, const Entry* replacement) {
    Node node = root ? load(root) : Node{};
    auto it = std::lower_bound(node.entries.begin(), node.entries.end(), key,
                               [](const Entry& x, int64_t y){ return x.key < y; });
    if (node.leaf) {
      if (it != node.entries.end() && it->key == key) {
        if (replacement) *it = *replacement; else node.entries.erase(it);
      } else if (replacement) node.entries.insert(it, *replacement);
    } else {
      if (it == node.entries.end()) --it;
      size_t index = size_t(it - node.entries.begin());
      auto children = change(it->position, key, replacement);
      node.entries.erase(node.entries.begin() + index);
      node.entries.insert(node.entries.begin() + index, children.begin(), children.end());
    }
    if (node.entries.empty()) return {};
    if (node.entries.size() <= capacity) {
      auto max = node.entries.back().key; return {{max, stage(root, std::move(node)), 0}};
    }
    Node right; right.leaf = node.leaf;
    auto mid = node.entries.begin() + node.entries.size() / 2;
    right.entries.assign(mid, node.entries.end()); node.entries.erase(mid, node.entries.end());
    auto left_max = node.entries.back().key, right_max = right.entries.back().key;
    return {{left_max, stage(root, std::move(node)), 0},
            {right_max, stage(0, std::move(right)), 0}};
  }
  uint64_t save(uint64_t root) {
    if (!root || root < temporary) return root;
    Node node = dirty_.at(root);
    if (!node.leaf) for (auto& e : node.entries) e.position = save(e.position);
    std::array<unsigned char, 4096> b{};
    write64(b.data(), 0x4D56444250414745ULL); write64(b.data() + 8, node.leaf);
    write64(b.data() + 16, node.entries.size());
    for (size_t i = 0; i < node.entries.size(); ++i) {
      auto* p = b.data() + 24 + i * 24; auto e = node.entries[i];
      write64(p, uint64_t(e.key)); write64(p + 8, e.position); write64(p + 16, e.length);
    }
    write64(b.data() + 4088, checksum(b.data(), 4088));
    return file_.append(b.data(), b.size());
  }
 public:
  explicit Tree(File& f) : file_(f) {}
  bool get(uint64_t root, int64_t key, Entry& result) {
    while (root) {
      auto node = load(root);
      auto it = std::lower_bound(node.entries.begin(), node.entries.end(), key,
                                 [](const Entry& x, int64_t y){ return x.key < y; });
      if (it == node.entries.end()) return false;
      if (node.leaf) { if (it->key != key) return false; result = *it; return true; }
      root = it->position;
    }
    return false;
  }
  uint64_t put(uint64_t root, Entry e) {
    auto roots = change(root, e.key, &e);
    if (roots.size() == 1) return roots[0].position;
    Node parent; parent.leaf = false; parent.entries = roots;
    return stage(0, std::move(parent));
  }
  uint64_t erase(uint64_t root, int64_t key) {
    auto roots = change(root, key, nullptr);
    return roots.empty() ? 0 : roots[0].position;
  }
  bool scan(uint64_t root, const std::function<bool(const Entry&)>& visitor) {
    if (!root) return true;
    auto node = load(root);
    for (auto e : node.entries) {
      if (node.leaf) { if (!visitor(e)) return false; }
      else if (!scan(e.position, visitor)) return false;
    }
    return true;
  }
  uint64_t checkpoint(uint64_t root) {
    root = save(root); dirty_.clear(); return root;
  }
  size_t dirty_count() const { return dirty_.size(); }
  void reset() { dirty_.clear(); cache_.clear(); }
};
} // namespace mvdb
