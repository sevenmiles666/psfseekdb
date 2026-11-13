#include "database.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <numeric>
#include <set>
#include <system_error>
#include <unistd.h>
#include <utility>

#if defined(__x86_64__) && defined(__GNUC__)
#include <immintrin.h>
#endif

namespace minivector {
namespace {

enum class RecordKind : std::uint8_t {
  Create = 1, Put = 2, Delete = 3, Commit = 4, Index = 5, PutHalf = 6, PutPacked = 7, PutScaledByte = 8
};

std::optional<VectorEncoding> row_encoding(RecordKind kind) {
  switch (kind) {
    case RecordKind::Put: return VectorEncoding::Full;
    case RecordKind::PutHalf: return VectorEncoding::Half;
    case RecordKind::PutPacked: return VectorEncoding::Packed;
    case RecordKind::PutScaledByte: return VectorEncoding::ScaledByte;
    default: return std::nullopt;
  }
}

std::size_t column_index(const std::vector<Column> &columns, const std::string &name) {
  const auto found = std::find_if(columns.begin(), columns.end(), [&](const Column &column) {
    return column.name == name;
  });
  if (found == columns.end()) {
    throw SqlError("UNKNOWN_COLUMN", "unknown column " + name);
  }
  return static_cast<std::size_t>(found - columns.begin());
}

Value convert(const Column &column, Value value) {
  switch (column.type) {
    case Type::Bigint:
      if (std::holds_alternative<std::int64_t>(value)) {
        return value;
      }
      break;
    case Type::Varchar:
      if (const auto *text = std::get_if<std::string>(&value)) {
        if (unicode_length(*text) > column.width) {
          throw SqlError("VARCHAR_LENGTH", "value is too long for " + column.name);
        }
        return value;
      }
      break;
    case Type::Vector: {
      if (const auto *text = std::get_if<std::string>(&value)) {
        value = parse_vector(*text);
      }
      if (const auto *vector = std::get_if<Vector>(&value)) {
        if (vector->size() != column.width) {
          throw SqlError("VECTOR_DIMENSION", "vector width does not match " + column.name);
        }
        return value;
      }
      break;
    }
  }
  throw SqlError("TYPE_MISMATCH", "value type does not match " + column.name);
}

template <class T>
int compare(const T &left, const T &right) {
  if (left < right) return -1;
  if (right < left) return 1;
  return 0;
}

int compare_values(const Value &left, const Value &right) {
  if (const auto *integer = std::get_if<std::int64_t>(&left)) {
    return compare(*integer, std::get<std::int64_t>(right));
  }
  if (const auto *text = std::get_if<std::string>(&left)) {
    // Lexicographic comparison of valid UTF-8 bytes preserves codepoint order.
    return text->compare(std::get<std::string>(right));
  }
  return compare(std::get<double>(left), std::get<double>(right));
}

struct BoundPredicate {
  std::size_t column;
  Comparison comparison;
  Value value;
};

std::vector<BoundPredicate> bind_predicates(const std::vector<Column> &columns,
                                           const std::vector<Predicate> &predicates) {
  std::vector<BoundPredicate> result;
  for (const Predicate &predicate : predicates) {
    const std::size_t index = column_index(columns, predicate.column);
    const Type type = columns[index].type;
    const bool integer = type == Type::Bigint && std::holds_alternative<std::int64_t>(predicate.value);
    const bool text = type == Type::Varchar && std::holds_alternative<std::string>(predicate.value);
    if (!integer && !text) {
      throw SqlError("TYPE_MISMATCH", "WHERE requires a scalar literal matching the column type");
    }
    result.push_back({index, predicate.comparison, predicate.value});
  }
  return result;
}

template <class Compare>
bool matches(const std::vector<BoundPredicate> &predicates, Compare compare_row,
             const BoundPredicate *already_matched = nullptr) {
  for (const BoundPredicate &predicate : predicates) {
    if (&predicate == already_matched) continue;
    const int order = compare_row(predicate);
    switch (predicate.comparison) {
      case Comparison::Eq: if (order != 0) return false; break;
      case Comparison::Lt: if (order >= 0) return false; break;
      case Comparison::Le: if (order > 0) return false; break;
      case Comparison::Gt: if (order <= 0) return false; break;
      case Comparison::Ge: if (order < 0) return false; break;
    }
  }
  return true;
}

bool matches(const Row &row, const std::vector<BoundPredicate> &predicates) {
  return matches(predicates, [&](const BoundPredicate &predicate) {
    return compare_values(row[predicate.column], predicate.value);
  });
}

std::optional<std::int64_t> primary_lookup(std::size_t primary,
                                          const std::vector<BoundPredicate> &predicates) {
  for (const BoundPredicate &predicate : predicates) {
    if (predicate.column == primary && predicate.comparison == Comparison::Eq) {
      return std::get<std::int64_t>(predicate.value);
    }
  }
  return std::nullopt;
}

struct BoundExpression {
  std::size_t column;
  const Vector *query;
  std::string name;
};

BoundExpression bind_expression(const std::vector<Column> &columns, const Expression &expression) {
  const std::size_t index = column_index(columns, expression.column);
  if (expression.query && columns[index].type != Type::Vector) {
    throw SqlError("TYPE_MISMATCH", "L2_DISTANCE requires a vector column");
  }
  if (expression.query && expression.query->size() != columns[index].width) {
    throw SqlError("VECTOR_DIMENSION", "query dimension does not match " + expression.column);
  }
  const std::string default_name = expression.query ? "L2_DISTANCE" : columns[index].name;
  return {index, expression.query ? &*expression.query : nullptr,
          expression.alias.empty() ? default_name : expression.alias};
}

double squared_distance(const float *left, const Vector &right) {
  double sum = 0;
  for (std::size_t i = 0; i < right.size(); ++i) {
    const double difference = static_cast<double>(left[i]) - static_cast<double>(right[i]);
    sum += difference * difference;
  }
  return sum;
}

using DistanceKernel = double (*)(const float *, const float *, std::size_t);

struct DistanceEstimate {
  DistanceKernel kernel = nullptr;
  double relative_margin = 1.0;
  double absolute_margin = 0.0;
};

constexpr std::size_t quantized_block_rows = 8;
constexpr std::size_t quantized_row_bytes = 4;

std::size_t quantized_offset(std::size_t row, std::size_t column, std::size_t dimension,
                             std::size_t block_rows) {
  if (row >= block_rows) return row * dimension + column;
  const auto lane = row % quantized_block_rows;
  return (row - lane) * dimension +
      (column / quantized_row_bytes * quantized_block_rows + lane) * quantized_row_bytes +
      column % quantized_row_bytes;
}

#if defined(__x86_64__) && defined(__GNUC__)
__attribute__((target("avx2,no-fma")))
double estimate_squared_distance(const float *left, const float *right, std::size_t size) {
  auto sum_low = _mm256_setzero_pd();
  auto sum_high = _mm256_setzero_pd();
  std::size_t i = 0;
  for (; i + 8 <= size; i += 8) {
    const auto low = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(left + i)),
                                  _mm256_cvtps_pd(_mm_loadu_ps(right + i)));
    const auto high = _mm256_sub_pd(_mm256_cvtps_pd(_mm_loadu_ps(left + i + 4)),
                                   _mm256_cvtps_pd(_mm_loadu_ps(right + i + 4)));
    sum_low = _mm256_add_pd(sum_low, _mm256_mul_pd(low, low));
    sum_high = _mm256_add_pd(sum_high, _mm256_mul_pd(high, high));
  }
  double lanes[4];
  _mm256_storeu_pd(lanes, _mm256_add_pd(sum_low, sum_high));
  double sum = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
  for (; i < size; ++i) {
    const double difference = static_cast<double>(left[i]) - static_cast<double>(right[i]);
    sum += difference * difference;
  }
  return sum;
}
__attribute__((target("avx2,fma")))
double estimate_squared_distance_f32(const float *left, const float *right, std::size_t size) {
  auto sum0 = _mm256_setzero_ps();
  auto sum1 = _mm256_setzero_ps();
  auto sum2 = _mm256_setzero_ps();
  auto sum3 = _mm256_setzero_ps();
  std::size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    const auto d0 = _mm256_sub_ps(_mm256_loadu_ps(left + i), _mm256_loadu_ps(right + i));
    const auto d1 = _mm256_sub_ps(_mm256_loadu_ps(left + i + 8), _mm256_loadu_ps(right + i + 8));
    const auto d2 = _mm256_sub_ps(_mm256_loadu_ps(left + i + 16), _mm256_loadu_ps(right + i + 16));
    const auto d3 = _mm256_sub_ps(_mm256_loadu_ps(left + i + 24), _mm256_loadu_ps(right + i + 24));
    sum0 = _mm256_fmadd_ps(d0, d0, sum0);
    sum1 = _mm256_fmadd_ps(d1, d1, sum1);
    sum2 = _mm256_fmadd_ps(d2, d2, sum2);
    sum3 = _mm256_fmadd_ps(d3, d3, sum3);
  }
  for (; i + 8 <= size; i += 8) {
    const auto diff = _mm256_sub_ps(_mm256_loadu_ps(left + i), _mm256_loadu_ps(right + i));
    sum0 = _mm256_fmadd_ps(diff, diff, sum0);
  }
  const auto sums = _mm256_add_ps(_mm256_add_ps(sum0, sum1), _mm256_add_ps(sum2, sum3));
  auto total = _mm_add_ps(_mm256_castps256_ps128(sums), _mm256_extractf128_ps(sums, 1));
  total = _mm_add_ps(total, _mm_movehl_ps(total, total));
  total = _mm_add_ss(total, _mm_shuffle_ps(total, total, 1));
  double result = _mm_cvtss_f32(total);
  // A float overflow requires a scalar double comparison.
  if (!std::isfinite(result)) return 0.0;
  for (; i < size; ++i) {
    const double diff = static_cast<double>(left[i]) - static_cast<double>(right[i]);
    result += diff * diff;
  }
  return result;
}
__attribute__((target("avx2")))
std::int32_t quantized_dot(const std::int8_t *left, const std::int8_t *right, std::size_t size) {
  auto lanes = _mm256_setzero_si256();
  const auto ones = _mm256_set1_epi16(1);
  std::size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    const auto x = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(left + i));
    const auto y = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(right + i));
    // Two products of components in [-127,127] fit in each int16 lane.
    const auto pairs = _mm256_maddubs_epi16(_mm256_abs_epi8(y), _mm256_sign_epi8(x, y));
    lanes = _mm256_add_epi32(lanes, _mm256_madd_epi16(pairs, ones));
  }
  auto sum = _mm_add_epi32(_mm256_castsi256_si128(lanes), _mm256_extracti128_si256(lanes, 1));
  sum = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum, sum);
  std::int32_t result = _mm_cvtsi128_si32(sum);
  for (; i < size; ++i) {
    result += int(left[i]) * int(right[i]);
  }
  return result;
}

__attribute__((target("avx2")))
__m256i quantized_block_dots(const std::int8_t *vectors, const std::int8_t *query, std::size_t size) {
  const auto ones = _mm256_set1_epi16(1);
  auto sum = _mm256_setzero_si256();
  for (std::size_t i = 0; i < size; i += quantized_row_bytes) {
    std::int32_t packed;
    std::memcpy(&packed, query + i, sizeof(packed));
    const auto q = _mm256_set1_epi32(packed);
    const auto rows = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(vectors + quantized_block_rows * i));
    const auto pairs = _mm256_maddubs_epi16(_mm256_abs_epi8(q), _mm256_sign_epi8(rows, q));
    sum = _mm256_add_epi32(sum, _mm256_madd_epi16(pairs, ones));
  }
  return sum;
}

__attribute__((target("avx2")))
unsigned quantized_block_mask(const std::int8_t *vectors, const std::int32_t *norms,
                              const std::int8_t *query, std::size_t size, std::int32_t cutoff,
                              std::int32_t *distances) {
  const auto sum = quantized_block_dots(vectors, query, size);
  const auto distance = _mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(norms)),
                                        _mm256_add_epi32(sum, sum));
  const auto excluded = _mm256_cmpgt_epi32(distance, _mm256_set1_epi32(cutoff));
  const auto mask = static_cast<unsigned>(~_mm256_movemask_ps(_mm256_castsi256_ps(excluded))) & 255;
  if (mask != 0) _mm256_storeu_si256(reinterpret_cast<__m256i *>(distances), distance);
  return mask;
}

__attribute__((target("avx2")))
std::int32_t quantized_block_row_dot(const std::int8_t *block, std::size_t row,
                                    const std::int8_t *query, std::size_t size) {
  const auto ones = _mm256_set1_epi16(1);
  auto sum = _mm256_setzero_si256();
  const auto *row_start = block + quantized_row_bytes * row;
  for (std::size_t i = 0; i < size; i += 32) {
    // Four coordinates from one row occupy one word in each interleaved group.
    const auto load = [&](std::size_t coordinate) {
      std::int32_t word;
      std::memcpy(&word, row_start + quantized_block_rows * coordinate, sizeof(word));
      return word;
    };
    const auto values = _mm256_setr_epi32(load(i), load(i + 4), load(i + 8), load(i + 12),
                                         load(i + 16), load(i + 20), load(i + 24), load(i + 28));
    const auto q = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(query + i));
    const auto pairs = _mm256_maddubs_epi16(_mm256_abs_epi8(q), _mm256_sign_epi8(values, q));
    sum = _mm256_add_epi32(sum, _mm256_madd_epi16(pairs, ones));
  }
  auto total = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
  total = _mm_hadd_epi32(total, total);
  return _mm_cvtsi128_si32(_mm_hadd_epi32(total, total));
}
#endif

using QuantizedKernel = std::int32_t (*)(const std::int8_t *, const std::int8_t *, std::size_t);

using BlockKernel = unsigned (*)(const std::int8_t *, const std::int32_t *, const std::int8_t *,
                                  std::size_t, std::int32_t, std::int32_t *);
using BlockRowKernel = std::int32_t (*)(const std::int8_t *, std::size_t, const std::int8_t *, std::size_t);

struct QuantizedBlockKernels {
  BlockKernel block = nullptr;
  BlockRowKernel row = nullptr;
};

QuantizedBlockKernels block_kernels() {
#if defined(__x86_64__) && defined(__GNUC__)
  if (__builtin_cpu_supports("avx2")) return {quantized_block_mask, quantized_block_row_dot};
#endif
  return {};
}

QuantizedKernel integer_distance_kernel() {
#if defined(__x86_64__) && defined(__GNUC__)
  return __builtin_cpu_supports("avx2") ? quantized_dot : nullptr;
#else
  return nullptr;
#endif
}

std::int8_t quantize_component(float value, double step) {
  return static_cast<std::int8_t>(std::lround(static_cast<double>(value) / step));
}

double quantization_error_bound(double squared_error, std::size_t dimension) {
  const auto epsilon = std::numeric_limits<double>::epsilon();
  // Bound norm accumulation and division roundoff for coordinates in [-127,127].
  return std::sqrt(squared_error) * (1.0 + (dimension + 8.0) * epsilon) +
         std::sqrt(static_cast<double>(dimension)) * 256.0 * epsilon;
}

DistanceEstimate distance_estimator(std::size_t dimension) {
#if defined(__x86_64__) && defined(__GNUC__)
  if (dimension >= 32 && __builtin_cpu_supports("avx2")) {
    const double rounding_terms = 4.0 * dimension + 32.0;
    if (dimension <= 4096 && __builtin_cpu_supports("fma")) {
      // Bound rounding in the differences and sum, including subnormal terms.
      // Only use this estimate to reject a row outside the expanded cutoff.
      return {estimate_squared_distance_f32,
              1.0 + rounding_terms * std::numeric_limits<float>::epsilon(),
              rounding_terms * std::numeric_limits<float>::denorm_min()};
    }
    return {estimate_squared_distance,
            1.0 + rounding_terms * std::numeric_limits<double>::epsilon(), 0.0};
  }
#else
  (void)dimension;
#endif
  return {};
}

Value evaluate(const BoundExpression &expression, const Row &row) {
  const Value &value = row[expression.column];
  if (!expression.query) {
    return value;
  }
  return std::sqrt(squared_distance(std::get<Vector>(value).data(), *expression.query));
}

// Runs contain only row references. Their keys are read from the journal during
// merge, keeping the spill file small even when VARCHAR keys are large.
class SortFile {
public:
  explicit SortFile(const std::filesystem::path &directory) {
    std::string path = (directory / ".mvdb-sort-XXXXXX").string();
    descriptor_ = ::mkostemp(path.data(), O_CLOEXEC);
    if (descriptor_ < 0) {
      fail("create sort file");
    }
    // Keep the file on the data device and release it automatically on SIGKILL.
    if (::unlink(path.c_str()) != 0) {
      const int error = errno;
      ::close(descriptor_);
      errno = error;
      fail("unlink sort file");
    }
  }

  ~SortFile() { ::close(descriptor_); }
  SortFile(const SortFile &) = delete;
  SortFile &operator=(const SortFile &) = delete;

  std::uint64_t size() const { return size_; }

  void append(std::int64_t key, RecordRef record) {
    std::array<std::uint64_t, 3> bytes{0, record.offset, record.size};
    std::memcpy(bytes.data(), &key, sizeof(key));
    ssize_t written = 0;
    do {
      written = ::write(descriptor_, bytes.data(), sizeof(bytes));
    } while (written < 0 && errno == EINTR);
    if (written != static_cast<ssize_t>(sizeof(bytes))) {
      if (written >= 0) errno = EIO;
      fail("write sort run");
    }
    ++size_;
  }

  std::pair<std::int64_t, RecordRef> read(std::uint64_t index) const {
    std::array<std::uint64_t, 3> bytes{};
    if (index >= size_ || index > static_cast<std::uint64_t>(
                                     std::numeric_limits<off_t>::max()) / sizeof(bytes)) {
      throw std::runtime_error("invalid sort run offset");
    }
    ssize_t length = 0;
    do {
      length = ::pread(descriptor_, bytes.data(), sizeof(bytes),
                       static_cast<off_t>(index * sizeof(bytes)));
    } while (length < 0 && errno == EINTR);
    if (length != static_cast<ssize_t>(sizeof(bytes))) {
      if (length >= 0) errno = EIO;
      fail("read sort run");
    }
    std::int64_t key = 0;
    std::memcpy(&key, bytes.data(), sizeof(key));
    return {key, {bytes[1], static_cast<std::uint32_t>(bytes[2])}};
  }

private:
  [[noreturn]] static void fail(const char *message) {
    throw std::system_error(errno, std::generic_category(), message);
  }

  int descriptor_;
  std::uint64_t size_ = 0;
};

}  // namespace

Database::Database(const std::filesystem::path &directory)
    : directory_(directory), journal_(directory_) {
  recover();
}

Database::Savepoint Database::savepoint() const { return {journal_.size(), undo_.size()}; }

void Database::rollback(Savepoint point, bool replay) {
  while (undo_.size() > point.undo_count) {
    const Undo undo = undo_.back();
    undo_.pop_back();
    Table &target = *tables_[undo.table];
    if (undo.kind == UndoKind::Table) {
      invalidate_cache(target.id);
      names_.erase(target.name);
      tables_.pop_back();
    } else if (undo.kind == UndoKind::Index) {
      target.index.reset();
    } else {
      set_row(target, undo.key, undo.previous, replay);
    }
  }
  if (journal_.size() != point.offset) {
    journal_.truncate(point.offset);
  }
}

void Database::commit(Savepoint point) {
  if (journal_.size() != point.offset) {
    journal_.append(Bytes{static_cast<std::uint8_t>(RecordKind::Commit)});
    journal_.sync();
  }
  undo_.resize(point.undo_count);
}

void Database::recover() {
  std::uint64_t next = Journal::records_begin;
  std::uint64_t committed = next;
  RecordRef record{};
  Bytes payload;
  while (journal_.read_frame(next, record, payload)) {
    next = record.offset + record.size;
    if (apply_record(payload, record, true)) {
      committed = next;
    }
  }
  if (committed != journal_.size()) {
    rollback({committed, 0}, true);
    journal_.sync();
  }
  for (const auto &target : tables_) {
    if (target->index) {
      rebuild_index(*target);
    }
  }
}

bool Database::apply_record(const Bytes &payload, RecordRef record, bool replay) {
  Reader input(payload);
  const auto kind = static_cast<RecordKind>(input.byte());
  if (kind == RecordKind::Commit) {
    input.finish();
    undo_.clear();
    return true;
  }
  const std::uint32_t id = input.u32();
  if (kind == RecordKind::Create) {
    if (id != tables_.size()) {
      throw std::runtime_error("invalid table sequence in journal");
    }
    std::string name = input.string();
    const std::uint32_t primary = input.u32();
    const std::uint32_t count = input.u32();
    if (primary >= count || count > input.remaining() / 9 || names_.count(name) != 0) {
      throw std::runtime_error("invalid table definition in journal");
    }
    std::vector<Column> columns;
    columns.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      std::string column = input.string();
      const auto type = static_cast<Type>(input.byte());
      const std::uint32_t width = input.u32();
      if (type != Type::Bigint && type != Type::Varchar && type != Type::Vector) {
        throw std::runtime_error("invalid column type in journal");
      }
      columns.push_back({std::move(column), type, width});
    }
    input.finish();
    if (columns[primary].type != Type::Bigint) {
      throw std::runtime_error("invalid primary key in journal");
    }
    names_.emplace(name, id);
    tables_.push_back(std::make_unique<Table>(
        Table{id, std::move(name), std::move(columns), primary, {}, std::nullopt}));
    undo_.push_back({id, UndoKind::Table, 0, std::nullopt});
    return false;
  }
  if (id >= tables_.size()) {
    throw std::runtime_error("mutation references an unknown table in journal");
  }
  Table &target = *tables_[id];
  if (kind == RecordKind::Index) {
    std::string name = input.string();
    const std::uint32_t column = input.u32();
    input.finish();
    if (target.index || column >= target.columns.size() || target.columns[column].type != Type::Vector) {
      throw std::runtime_error("invalid vector index definition in journal");
    }
    target.index.emplace(VectorIndex{std::move(name), column, {}});
    undo_.push_back({id, UndoKind::Index, 0, std::nullopt});
    return false;
  }
  const auto encoding = row_encoding(kind);
  if (!encoding && kind != RecordKind::Delete) {
    throw std::runtime_error("invalid mutation in journal");
  }
  const std::int64_t key = input.i64();
  const auto existing = target.rows.find(key);
  std::optional<StoredRow> previous;
  if (existing != target.rows.end()) {
    previous = existing->second;
  }
  undo_.push_back({id, UndoKind::Row, key, previous});
  if (encoding) {
    const float projection = target.index && !replay ? read_projection(target, input, *encoding) : 0;
    set_row(target, key, StoredRow{record, projection}, replay);
  } else {
    input.finish();
    set_row(target, key, std::nullopt, replay);
  }
  return false;
}

void Database::write_change(const Bytes &payload) {
  const RecordRef record = journal_.append(payload);
  apply_record(payload, record);
}

void Database::transaction(Transaction command) {
  if (command == Transaction::Begin) {
    if (transaction_) {
      throw SqlError("TRANSACTION_ACTIVE", "a transaction is already active");
    }
    transaction_ = savepoint();
    return;
  }
  if (!transaction_) {
    throw SqlError("NO_TRANSACTION", "no explicit transaction is active");
  }
  if (command == Transaction::Commit) {
    commit(*transaction_);
  } else {
    rollback(*transaction_);
  }
  transaction_.reset();
}

void Database::close() {
  if (transaction_) {
    rollback(*transaction_);
    transaction_.reset();
  }
}

Database::Table &Database::table(const std::string &name) {
  const auto found = names_.find(name);
  if (found == names_.end()) {
    throw SqlError("UNKNOWN_TABLE", "unknown table " + name);
  }
  return *tables_[found->second];
}

float Database::read_projection(const Table &table, Reader &row, VectorEncoding encoding) const {
  // Row metadata and the primary key have already been consumed.
  for (std::size_t i = 0; i < table.index->column; ++i) {
    if (i == table.primary_key) continue;
    if (table.columns[i].type == Type::Bigint) {
      row.skip(sizeof(std::int64_t));
    } else {
      const std::uint32_t bytes = row.u32();
      row.skip(bytes);
    }
  }
  float projection;
  row.vector(&projection, 1, encoding);
  if (!std::isfinite(projection)) {
    throw std::runtime_error("invalid vector projection in journal");
  }
  return projection;
}

void Database::rebuild_index(Table &table) {
  Bytes buffer;
  auto &entries = table.index->ordered;
  entries.clear();
  for (auto &row : table.rows) {
    journal_.read(row.second.reference, buffer);
    Reader input(buffer);
    const auto encoding = row_encoding(static_cast<RecordKind>(input.byte()));
    if (!encoding || input.u32() != table.id ||
        input.i64() != row.first) {
      throw std::runtime_error("row reference does not match vector index");
    }
    row.second.projection = read_projection(table, input, *encoding);
    entries.emplace(row.second.projection, row.first);
  }
}

void Database::set_row(Table &table, std::int64_t key, std::optional<StoredRow> row, bool replay) {
  invalidate_cache(table.id);
  const auto current = table.rows.find(key);
  const bool maintain = table.index && !replay;
  if (maintain && current != table.rows.end()) {
    table.index->ordered.erase({current->second.projection, key});
  }
  if (row) {
    table.rows.insert_or_assign(key, *row);
    if (maintain) {
      table.index->ordered.emplace(row->projection, key);
    }
  } else if (current != table.rows.end()) {
    table.rows.erase(current);
  }
}

void Database::invalidate_cache(std::uint32_t table) {
  if (search_cache_ && search_cache_->table == table) {
    search_cache_.reset();
  }
  if (oversized_cache_ && *oversized_cache_ == table) {
    oversized_cache_.reset();
  }
}

Database::SearchCache *Database::search_cache(const Table &table, std::size_t vector_column) {
  if (search_cache_ && search_cache_->table == table.id) {
    return &*search_cache_;
  }
  if (oversized_cache_ && *oversized_cache_ == table.id) {
    return nullptr;
  }
  constexpr std::uint64_t budget = 8 * 1024 * 1024;
  const std::uint64_t dimension = table.columns[vector_column].width;
  std::uint64_t fixed_per_row = sizeof(std::uint32_t) + dimension * sizeof(float);
  for (const auto &column : table.columns) {
    if (column.type == Type::Bigint) fixed_per_row += sizeof(std::int64_t);
    if (column.type == Type::Varchar) fixed_per_row += sizeof(std::string);
  }
  const auto count = table.rows.size();
  const std::uint64_t column_bytes = table.columns.size() * sizeof(CachedColumn);
  if (column_bytes > budget || count > (budget - column_bytes) / fixed_per_row) {
    oversized_cache_ = table.id;
    return nullptr;
  }

  search_cache_.reset();
  SearchCache cache{};
  cache.table = table.id;
  cache.keys.reserve(count);
  const std::size_t components = count * dimension;
  const auto quantized_bytes = components + count * sizeof(std::int32_t);
  const auto sign_bytes = (components + 7) / 8;
  const bool quantize = dimension >= 32 && dimension <= 4096 && components >= 256 * 1024 &&
                        integer_distance_kernel();
  // Try an exact byte representation first. If a row cannot share the common
  // scale, expand the preceding rows into the normal float cache once.
  cache.exact_codes = quantize && quantized_bytes + sign_bytes <=
      budget - column_bytes - count * (fixed_per_row - dimension * sizeof(float));
  if (!cache.exact_codes) cache.vectors.reserve(components);
  cache.scalars.resize(table.columns.size());
  for (std::size_t column = 0; column < table.columns.size(); ++column) {
    if (column == table.primary_key || column == vector_column) continue;
    if (table.columns[column].type == Type::Bigint) {
      cache.scalars[column].emplace<std::vector<std::int64_t>>().reserve(count);
    } else {
      cache.scalars[column].emplace<std::vector<std::string>>().reserve(count);
    }
  }
  std::uint64_t used = column_bytes + count * fixed_per_row;
  if (cache.exact_codes) used = used - components * sizeof(float) + quantized_bytes + sign_bytes;
  Bytes buffer;
  Row row;
  std::vector<std::int8_t> codes;
  if (cache.exact_codes) codes.resize(dimension);
  float max_abs = 0;
  float inverse = 0;
  for (const auto &entry : table.rows) {
    load_row(table, entry.second.reference, buffer, row);
    const auto &vector = std::get<Vector>(row[vector_column]);
    const auto row_index = cache.keys.size();
    if (quantize) {
      for (const float value : vector) max_abs = std::max(max_abs, std::abs(value));
    }
    if (cache.exact_codes) {
      if (inverse == 0 && max_abs != 0) {
        const auto exponent = scaled_byte_exponent(float_bits(max_abs));
        if (exponent >= -126 && exponent <= 121) {
          inverse = float_from_bits(std::uint32_t(127 - exponent) << 23);
          cache.quantization_step = 1.0 / inverse;
        }
      }
      bool exact = inverse != 0 || max_abs == 0;
      std::int32_t norm = 0;
      for (std::size_t i = 0; exact && i < dimension; ++i) {
        int code;
        exact = exact_scaled_byte(vector[i], inverse, code);
        if (!exact) break;
        codes[i] = static_cast<std::int8_t>(code);
        norm += code * code;
      }
      if (exact) {
        if (cache.quantized.empty()) {
          cache.quantized.resize(components);
          cache.negative_zeros.resize(sign_bytes);
          cache.quantized_norms.resize(count);
          if (dimension % 32 == 0) cache.block_rows = count - count % quantized_block_rows;
        }
        for (std::size_t i = 0; i < dimension; ++i) {
          cache.quantized[quantized_offset(row_index, i, dimension, cache.block_rows)] = codes[i];
          if (float_bits(vector[i]) == 0x80000000U) {
            const auto index = row_index * dimension + i;
            cache.negative_zeros[index / 8] |= static_cast<std::uint8_t>(1U << (index % 8));
          }
        }
        cache.quantized_norms[row_index] = norm;
      } else {
        used = used - quantized_bytes - sign_bytes + components * sizeof(float);
        if (used > budget) {
          oversized_cache_ = table.id;
          return nullptr;
        }
        cache.vectors.reserve(components);
        for (std::size_t previous = 0; previous < row_index; ++previous) {
          for (std::size_t i = 0; i < dimension; ++i) {
            const auto index = previous * dimension + i;
            const auto code = cache.quantized[quantized_offset(previous, i, dimension, cache.block_rows)];
            const bool negative_zero = code == 0 && (cache.negative_zeros[index / 8] & (1U << (index % 8)));
            cache.vectors.push_back(negative_zero ? -0.0f : code * static_cast<float>(cache.quantization_step));
          }
        }
        cache.exact_codes = false;
        std::vector<std::uint8_t>{}.swap(cache.negative_zeros);
      }
    }
    if (!cache.exact_codes) cache.vectors.insert(cache.vectors.end(), vector.begin(), vector.end());
    for (std::size_t column = 0; column < row.size(); ++column) {
      if (column == vector_column || column == table.primary_key) continue;
      if (const auto *text = std::get_if<std::string>(&row[column])) {
        const std::uint64_t bytes = text->size() + 1;
        if (bytes > budget - used) {
          oversized_cache_ = table.id;
          return nullptr;
        }
        used += bytes;
        std::get<std::vector<std::string>>(cache.scalars[column]).push_back(*text);
      } else {
        std::get<std::vector<std::int64_t>>(cache.scalars[column]).push_back(std::get<std::int64_t>(row[column]));
      }
    }
    cache.keys.push_back(entry.first);
  }
  if (cache.exact_codes && max_abs == 0) cache.quantization_step = 1.0;
  if (!cache.exact_codes) {
    if (quantize && quantized_bytes <= budget - used && max_abs != 0) {
      used += quantized_bytes;
      cache.quantization_step = static_cast<double>(max_abs) / 127.0;
      cache.quantized.resize(components);
      cache.quantized_norms.resize(count);
      // Interleave complete eight-row blocks in the original allocation.
      if (dimension % 32 == 0) cache.block_rows = count - count % quantized_block_rows;
      double maximum_error_squared = 0.0;
      for (std::size_t row = 0; row < count; ++row) {
        const auto *values = cache.vectors.data() + row * dimension;
        const bool interleaved = row < cache.block_rows;
        const std::size_t lane = row % quantized_block_rows;
        auto *base = cache.quantized.data() + (interleaved ? row - lane : row) * dimension;
        std::int32_t norm = 0;
        double error_squared = 0.0;
        for (std::size_t i = 0; i < dimension; ++i) {
          const double scaled = static_cast<double>(values[i]) / cache.quantization_step;
          const auto value = static_cast<std::int8_t>(std::lround(scaled));
          const std::size_t offset = interleaved ?
              (i / quantized_row_bytes * quantized_block_rows + lane) * quantized_row_bytes +
                  i % quantized_row_bytes : i;
          base[offset] = value;
          norm += int(value) * int(value);
          const double error = scaled - value;
          error_squared += error * error;
        }
        cache.quantized_norms[row] = norm;
        maximum_error_squared = std::max(maximum_error_squared, error_squared);
      }
      cache.quantization_error = quantization_error_bound(maximum_error_squared, dimension);
    } else {
      cache.block_rows = 0;
      std::vector<std::int8_t>{}.swap(cache.quantized);
      std::vector<std::int32_t>{}.swap(cache.quantized_norms);
    }
  }
  cache.can_group_quantized = cache.block_rows != 0 && count * sizeof(std::uint32_t) <= budget - used;
  search_cache_ = std::move(cache);
  return &*search_cache_;
}

void Database::load_row(const Table &table, RecordRef record, Bytes &buffer, Row &row) {
  journal_.read(record, buffer);
  Reader input(buffer);
  const auto encoding = row_encoding(static_cast<RecordKind>(input.byte()));
  if (!encoding || input.u32() != table.id) {
    throw std::runtime_error("row reference does not match the table");
  }
  const std::int64_t key = input.i64();
  row.resize(table.columns.size());
  for (std::size_t i = 0; i < table.columns.size(); ++i) {
    const Column &column = table.columns[i];
    if (i == table.primary_key) {
      row[i] = key;
    } else if (column.type == Type::Bigint) {
      row[i] = input.i64();
    } else if (column.type == Type::Varchar) {
      row[i] = input.string();
    } else {
      if (vector_bytes(*encoding, column.width) > input.remaining()) {
        throw std::runtime_error("incomplete vector in journal");
      }
      if (!std::holds_alternative<Vector>(row[i])) {
        row[i] = Vector{};
      }
      auto &vector = std::get<Vector>(row[i]);
      vector.resize(column.width);
      input.vector(vector.data(), vector.size(), *encoding);
    }
  }
  input.finish();
}

void Database::put_row(const Table &table, std::int64_t key, const Row &row, Bytes &payload) {
  payload.clear();
  payload.push_back(static_cast<std::uint8_t>(RecordKind::Put));
  put_u32(payload, table.id);
  put_i64(payload, key);
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i == table.primary_key) {
      continue;
    }
    switch (table.columns[i].type) {
      case Type::Bigint: put_i64(payload, std::get<std::int64_t>(row[i])); break;
      case Type::Varchar: put_string(payload, std::get<std::string>(row[i])); break;
      case Type::Vector: {
        const auto &vector = std::get<Vector>(row[i]);
        switch (put_vector(payload, vector.data(), vector.size())) {
          case VectorEncoding::Full: break;
          case VectorEncoding::Half: payload.front() = static_cast<std::uint8_t>(RecordKind::PutHalf); break;
          case VectorEncoding::Packed: payload.front() = static_cast<std::uint8_t>(RecordKind::PutPacked); break;
          case VectorEncoding::ScaledByte: payload.front() = static_cast<std::uint8_t>(RecordKind::PutScaledByte); break;
        }
        break;
      }
    }
  }
  write_change(payload);
}

std::uint64_t Database::create_table(const CreateTable &statement) {
  if (names_.count(statement.table) != 0) {
    throw SqlError("TABLE_EXISTS", "table already exists " + statement.table);
  }
  if (tables_.size() >= std::numeric_limits<std::uint32_t>::max() ||
      statement.columns.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw SqlError("INVALID_SCHEMA", "catalog exceeds the supported size");
  }
  Bytes payload{static_cast<std::uint8_t>(RecordKind::Create)};
  put_u32(payload, static_cast<std::uint32_t>(tables_.size()));
  put_string(payload, statement.table);
  put_u32(payload, static_cast<std::uint32_t>(statement.primary_key));
  put_u32(payload, static_cast<std::uint32_t>(statement.columns.size()));
  for (const Column &column : statement.columns) {
    put_string(payload, column.name);
    payload.push_back(static_cast<std::uint8_t>(column.type));
    put_u32(payload, column.width);
  }
  write_change(payload);
  return 0;
}

std::uint64_t Database::create_index(const CreateVectorIndex &statement) {
  Table &target = table(statement.table);
  const std::size_t column = column_index(target.columns, statement.column);
  if (target.columns[column].type != Type::Vector) {
    throw SqlError("TYPE_MISMATCH", "CREATE VECTOR INDEX requires a vector column");
  }
  if (target.index) {
    throw SqlError("INDEX_EXISTS", "table already has vector index " + target.index->name);
  }
  Bytes payload{static_cast<std::uint8_t>(RecordKind::Index)};
  put_u32(payload, target.id);
  put_string(payload, statement.name);
  put_u32(payload, static_cast<std::uint32_t>(column));
  write_change(payload);
  rebuild_index(target);
  return 0;
}

std::uint64_t Database::insert(Insert &statement) {
  Table &target = table(statement.table);
  std::vector<std::size_t> positions(target.columns.size());
  if (statement.columns.empty()) {
    std::iota(positions.begin(), positions.end(), 0);
  } else {
    if (statement.columns.size() != target.columns.size()) {
      throw SqlError("COLUMN_COUNT", "INSERT must provide each column exactly once");
    }
    std::set<std::size_t> distinct;
    for (std::size_t i = 0; i < positions.size(); ++i) {
      positions[i] = column_index(target.columns, statement.columns[i]);
      if (!distinct.insert(positions[i]).second) {
        throw SqlError("COLUMN_COUNT", "duplicate INSERT column");
      }
    }
  }
  Row values(target.columns.size());
  Bytes payload;
  for (Row &row : statement.rows) {
    if (row.size() != positions.size()) {
      throw SqlError("COLUMN_COUNT", "INSERT value count does not match the table");
    }
    for (std::size_t i = 0; i < positions.size(); ++i) {
      values[positions[i]] = convert(target.columns[positions[i]], std::move(row[i]));
    }
    const std::int64_t key = std::get<std::int64_t>(values[target.primary_key]);
    if (target.rows.count(key) != 0) {
      throw SqlError("DUPLICATE_KEY", "duplicate primary key " + std::to_string(key));
    }
    put_row(target, key, values, payload);
  }
  return statement.rows.size();
}

std::uint64_t Database::update(const Update &statement) {
  Table &target = table(statement.table);
  const auto predicates = bind_predicates(target.columns, statement.predicates);
  std::map<std::size_t, Value> changes;
  for (const Assignment &assignment : statement.assignments) {
    const std::size_t index = column_index(target.columns, assignment.column);
    if (index == target.primary_key) {
      throw SqlError("UNSUPPORTED", "primary key updates are unsupported");
    }
    if (!changes.emplace(index, convert(target.columns[index], assignment.value)).second) {
      throw SqlError("COLUMN_COUNT", "duplicate UPDATE column");
    }
  }
  const auto key = primary_lookup(target.primary_key, predicates);
  auto next = key ? target.rows.find(*key) : target.rows.begin();
  Bytes buffer;
  Bytes payload;
  Row row;
  std::uint64_t count = 0;
  while (next != target.rows.end()) {
    const auto current = next++;
    load_row(target, current->second.reference, buffer, row);
    if (matches(row, predicates)) {
      for (const auto &change : changes) {
        row[change.first] = change.second;
      }
      put_row(target, current->first, row, payload);
      ++count;
    }
    if (key) break;
  }
  return count;
}

std::uint64_t Database::remove(const Delete &statement) {
  Table &target = table(statement.table);
  const auto predicates = bind_predicates(target.columns, statement.predicates);
  const auto key = primary_lookup(target.primary_key, predicates);
  auto next = key ? target.rows.find(*key) : target.rows.begin();
  Bytes buffer;
  Row row;
  std::uint64_t count = 0;
  while (next != target.rows.end()) {
    const auto current = next++;
    if (!predicates.empty()) {
      load_row(target, current->second.reference, buffer, row);
    }
    if (matches(row, predicates)) {
      Bytes payload{static_cast<std::uint8_t>(RecordKind::Delete)};
      put_u32(payload, target.id);
      put_i64(payload, current->first);
      write_change(payload);
      ++count;
    }
    if (key) break;
  }
  return count;
}

Execution Database::select(const Select &statement, const RowSink &sink, SearchMode mode,
                           const ColumnsSink &columns_sink) {
  Table &target = table(statement.table);
  const auto predicates = bind_predicates(target.columns, statement.predicates);
  std::vector<BoundExpression> projection;
  if (statement.all_columns) {
    for (std::size_t i = 0; i < target.columns.size(); ++i) {
      projection.push_back({i, nullptr, target.columns[i].name});
    }
  } else {
    for (const Expression &expression : statement.columns) {
      projection.push_back(bind_expression(target.columns, expression));
    }
  }
  Execution result;
  for (const BoundExpression &expression : projection) {
    result.columns.push_back(expression.name);
  }

  std::vector<BoundExpression> order;
  for (const Order &key : statement.order) {
    const auto alias = std::find_if(statement.columns.begin(), statement.columns.end(),
                                    [&](const Expression &expression) {
      return !key.expression.query && !expression.alias.empty() &&
             expression.alias == key.expression.column;
    });
    auto expression = bind_expression(target.columns,
                                      alias == statement.columns.end() ? key.expression : *alias);
    if (!expression.query && target.columns[expression.column].type == Type::Vector) {
      throw SqlError("TYPE_MISMATCH", "ORDER BY a vector requires L2_DISTANCE");
    }
    order.push_back(std::move(expression));
  }
  if (columns_sink) {
    columns_sink(result.columns);
  }
  const auto limit = statement.limit.value_or(std::numeric_limits<std::uint64_t>::max());
  if (limit == 0) {
    return result;
  }
  const auto key = primary_lookup(target.primary_key, predicates);
  const bool in_key_order = order.empty() ||
      (!order.front().query && order.front().column == target.primary_key);
  Bytes buffer;
  Row row;
  Row projected;
  projected.reserve(projection.size());
  const auto emit = [&] {
    if (sink) {
      projected.clear();
      for (const BoundExpression &expression : projection) {
        projected.push_back(evaluate(expression, row));
      }
      sink(projected);
    }
    ++result.count;
  };
  const auto read_match = [&](RecordRef record) {
    load_row(target, record, buffer, row);
    return matches(row, predicates);
  };

  if (key) {
    const auto found = target.rows.find(*key);
    if (found != target.rows.end() && read_match(found->second.reference)) {
      emit();
    }
    return result;
  }
  if (in_key_order) {
    const auto scan = [&](auto current, auto end) {
      for (; current != end && result.count < limit; ++current) {
        if (read_match(current->second.reference)) {
          emit();
        }
      }
    };
    if (!statement.order.empty() && statement.order.front().descending) {
      scan(target.rows.rbegin(), target.rows.rend());
    } else {
      scan(target.rows.begin(), target.rows.end());
    }
    return result;
  }

  struct Neighbor {
    double distance;
    std::int64_t key;
    RecordRef reference;
    std::size_t index;
  };
  constexpr std::size_t sort_budget = 2 * 1024 * 1024;
  const bool primary_tiebreak = order.size() == 2 && !order[1].query && order[1].column == target.primary_key;
  if ((order.size() == 1 || primary_tiebreak) && order.front().query && !statement.order.front().descending &&
      limit <= sort_budget / sizeof(Neighbor)) {
    const auto vector_column = order.front().column;
    const Vector &query = *order.front().query;
    const bool descending_ties = primary_tiebreak && statement.order[1].descending;
    const auto closer = [&](const Neighbor &left, const Neighbor &right) {
      if (left.distance != right.distance) {
        return left.distance < right.distance;
      }
      return descending_ties ? left.key > right.key : left.key < right.key;
    };
    auto *cache = search_cache(target, vector_column);
    const auto estimate = distance_estimator(query.size());
    QuantizedKernel dot = nullptr;
    std::vector<std::int8_t> query_quantized;
    std::int32_t query_norm = 0;
    double error_radius = 0.0;
    QuantizedBlockKernels blocks;
    bool exact_query = false;
    if (cache && !cache->quantized.empty() &&
        std::all_of(query.begin(), query.end(), [&](float value) {
          return std::abs(value) <= 127.0 * cache->quantization_step;
        })) {
      dot = integer_distance_kernel();
      query_quantized.reserve(query.size());
      exact_query = cache->exact_codes;
      const float inverse = static_cast<float>(1.0 / cache->quantization_step);
      double query_error_squared = 0.0;
      for (const float value : query) {
        int code;
        if (exact_query && exact_scaled_byte(value, inverse, code)) {
          query_quantized.push_back(static_cast<std::int8_t>(code));
        } else {
          exact_query = false;
          query_quantized.push_back(quantize_component(value, cache->quantization_step));
        }
        const auto error = static_cast<double>(value) / cache->quantization_step - query_quantized.back();
        query_error_squared += error * error;
      }
      error_radius = cache->quantization_error + quantization_error_bound(query_error_squared, query.size());
      query_norm = dot(query_quantized.data(), query_quantized.data(), query.size());
      if (cache->block_rows != 0) blocks = block_kernels();
    }
    double cutoff = std::numeric_limits<double>::infinity();
    double worst_squared = cutoff;
    std::vector<Neighbor> nearest;
    nearest.reserve(std::min<std::uint64_t>(limit, target.rows.size()));
    const auto consider_distance = [&](std::int64_t key, RecordRef reference, double squared,
                                       std::size_t index = 0) {
      Neighbor candidate{std::sqrt(squared), key, reference, index};
      if (nearest.size() < limit) {
        nearest.push_back(candidate);
        std::push_heap(nearest.begin(), nearest.end(), closer);
      } else if (closer(candidate, nearest.front())) {
        std::pop_heap(nearest.begin(), nearest.end(), closer);
        nearest.back() = candidate;
        std::push_heap(nearest.begin(), nearest.end(), closer);
      } else {
        return;
      }
      if (nearest.size() == limit) {
        const double worst_distance = nearest.front().distance;
        // Widen the squared cutoff to retain all rows whose rounded L2 distance ties.
        worst_squared = (worst_distance * worst_distance) *
                        (1.0 + 4.0 * std::numeric_limits<double>::epsilon());
        if (dot) {
          // A shared power-of-two scale keeps integer sums exactly representable.
          if (exact_query) {
            cutoff = worst_squared / (cache->quantization_step * cache->quantization_step);
          } else {
            // The two rounding errors bound the difference between distance norms.
            const double radius = worst_distance / cache->quantization_step *
                                  (1.0 + (4.0 * query.size() + 4.0) * std::numeric_limits<double>::epsilon()) +
                                  error_radius;
            cutoff = radius * radius;
          }
        } else {
          cutoff = worst_squared * estimate.relative_margin + estimate.absolute_margin;
        }
      }
    };
    const auto read_candidate = [&](std::int64_t key, RecordRef reference) {
      if (read_match(reference)) {
        consider_distance(key, reference, squared_distance(std::get<Vector>(row[vector_column]).data(), query));
      }
    };

    Vector decoded;
    if (cache && cache->exact_codes) decoded.resize(query.size());
    const auto quantized_position = [&](std::size_t index) {
      if (cache->equality && !cache->equality->positions.empty()) {
        return static_cast<std::size_t>(cache->equality->positions[index]);
      }
      return index;
    };
    const auto cached_vector = [&](std::size_t index) -> const float * {
      const std::size_t offset = index * query.size();
      if (!cache->exact_codes) return cache->vectors.data() + offset;
      const auto position = quantized_position(index);
      const float scale = static_cast<float>(cache->quantization_step);
      for (std::size_t i = 0; i < query.size(); ++i) {
        const auto value = cache->quantized[quantized_offset(position, i, query.size(), cache->block_rows)];
        const auto component = offset + i;
        decoded[i] = value == 0 && (cache->negative_zeros[component / 8] & (1U << (component % 8)))
            ? -0.0f : value * scale;
      }
      return decoded.data();
    };
    const auto consider_cached = [&](std::size_t index, std::optional<std::int32_t> distance = {}) {
      if (dot && (exact_query || nearest.size() == limit)) {
        if (!distance) {
          const auto position = quantized_position(index);
          std::int32_t cross;
          if (position < cache->block_rows) {
            const auto lane = position % quantized_block_rows;
            const auto *block = cache->quantized.data() + (position - lane) * query.size();
            cross = blocks.row(block, lane, query_quantized.data(), query.size());
          } else {
            cross = dot(cache->quantized.data() + position * query.size(), query_quantized.data(), query.size());
          }
          distance = cache->quantized_norms[position] + query_norm - 2 * cross;
        }
        if (*distance > cutoff) return;
        if (exact_query) {
          const double scale = cache->quantization_step;
          consider_distance(cache->keys[index], {}, *distance * scale * scale, index);
          return;
        }
      }
      const auto *vector = cached_vector(index);
      if (!dot && nearest.size() == limit && estimate.kernel &&
          estimate.kernel(vector, query.data(), query.size()) > cutoff) return;
      consider_distance(cache->keys[index], {}, squared_distance(vector, query), index);
    };
    const auto compare_scalar = [&](std::size_t index, std::size_t column, const Value &value) {
      if (column == target.primary_key) return compare(cache->keys[index], std::get<std::int64_t>(value));
      const auto &values = cache->scalars[column];
      if (const auto *integers = std::get_if<std::vector<std::int64_t>>(&values)) {
        return compare((*integers)[index], std::get<std::int64_t>(value));
      }
      return std::get<std::vector<std::string>>(values)[index].compare(std::get<std::string>(value));
    };
    const auto matches_cached = [&](std::size_t index, const BoundPredicate *already_matched = nullptr) {
      return matches(predicates, [&](const BoundPredicate &predicate) {
        return compare_scalar(index, predicate.column, predicate.value);
      }, already_matched);
    };
    const auto scan_blocks = [&](std::size_t begin, std::size_t end, const BoundPredicate *already_matched) {
      const bool grouped = cache->equality && !cache->equality->positions.empty();
      const auto visit = [&](std::size_t position, std::optional<std::int32_t> distance = {}) {
        const auto index = grouped ? cache->equality->rows[position] : position;
        if (predicates.empty() || (already_matched && predicates.size() == 1) ||
            matches_cached(index, already_matched)) {
          consider_cached(index, distance);
        }
      };
      for (; begin < end && (begin % quantized_block_rows != 0 || nearest.size() < limit); ++begin) visit(begin);
      const auto block_end = std::min(end - end % quantized_block_rows, cache->block_rows);
      for (; begin < block_end; begin += quantized_block_rows) {
        const double bound = std::min(cutoff - query_norm, 254.0 * 254.0 * query.size());
        std::array<std::int32_t, quantized_block_rows> distances;
        auto mask = blocks.block(cache->quantized.data() + begin * query.size(),
                                 cache->quantized_norms.data() + begin, query_quantized.data(), query.size(),
                                 static_cast<std::int32_t>(std::ceil(bound)), distances.data());
        while (mask != 0) {
          const unsigned lane = static_cast<unsigned>(__builtin_ctz(mask));
          visit(begin + lane, distances[lane] + query_norm);
          mask &= mask - 1;
        }
      }
      for (; begin < end; ++begin) visit(begin);
    };

    if (mode == SearchMode::Ann && target.index && !primary_tiebreak) {
      const auto &entries = target.index->ordered;
      auto right = entries.lower_bound({query.front(), std::numeric_limits<std::int64_t>::min()});
      auto left = std::make_reverse_iterator(right);
      const auto lower_bound = [&](float projection) {
        const double difference = static_cast<double>(projection) - query.front();
        return difference * difference;
      };
      // Both sides are in increasing order of the one-coordinate lower bound.
      // A full heap permits a 0.1% relative distance relaxation in ANN mode.
      constexpr double approximation = 1.001 * 1.001;
      while (left != entries.rend() || right != entries.end()) {
        const bool from_left = right == entries.end() ||
            (left != entries.rend() && lower_bound(left->first) <= lower_bound(right->first));
        const auto entry = from_left ? *left++ : *right++;
        if (nearest.size() == limit &&
            lower_bound(entry.first) * approximation > worst_squared) {
          break;
        }
        if (cache) {
          const auto found = std::lower_bound(cache->keys.begin(), cache->keys.end(), entry.second);
          if (found == cache->keys.end() || *found != entry.second) {
            throw std::runtime_error("vector index references missing row");
          }
          const auto index = static_cast<std::size_t>(found - cache->keys.begin());
          if (matches_cached(index)) consider_cached(index);
        } else {
          read_candidate(entry.second, target.rows.at(entry.second).reference);
        }
      }
    } else if (cache) {
      const auto filter = std::find_if(predicates.begin(), predicates.end(), [&](const BoundPredicate &p) {
        return p.comparison == Comparison::Eq &&
               (!cache->equality || cache->equality->column == p.column);
      });
      if (filter == predicates.end()) {
        if (predicates.empty() && blocks.block) {
          scan_blocks(0, cache->keys.size(), nullptr);
        } else {
          for (std::size_t i = 0; i < cache->keys.size(); ++i) {
            if (predicates.empty() || matches_cached(i)) consider_cached(i);
          }
        }
      } else {
        if (!cache->equality) {
          CachedEquality index{filter->column, {}, {}};
          index.rows.resize(cache->keys.size());
          std::iota(index.rows.begin(), index.rows.end(), std::uint32_t{0});
          const auto sort = [&](const auto &values) {
            std::sort(index.rows.begin(), index.rows.end(), [&](auto left, auto right) {
              const int comparison = compare(values[left], values[right]);
              return comparison == 0 ? left < right : comparison < 0;
            });
          };
          const auto &values = cache->scalars[filter->column];
          if (const auto *integers = std::get_if<std::vector<std::int64_t>>(&values)) sort(*integers);
          else sort(std::get<std::vector<std::string>>(values));
          cache->equality.emplace(std::move(index));
        }
        const auto &indices = cache->equality->rows;
        const auto first = std::lower_bound(indices.begin(), indices.end(), filter->value,
            [&](auto index, const Value &value) { return compare_scalar(index, filter->column, value) < 0; });
        const auto end = std::upper_bound(first, indices.end(), filter->value,
            [&](const Value &value, auto index) { return compare_scalar(index, filter->column, value) > 0; });
        if (blocks.block && cache->can_group_quantized && cache->equality->positions.empty() &&
            end - first >= 4 * static_cast<std::ptrdiff_t>(quantized_block_rows)) {
          // Reorder the byte cache in place by the scalar key. Keep row identity
          // and the original vectors in primary-key order.
          auto &positions = cache->equality->positions;
          std::vector<bool> visited(indices.size());
          std::vector<std::int8_t> saved(query.size());
          positions.resize(indices.size());
          for (std::size_t i = 0; i < indices.size(); ++i) positions[indices[i]] = i;
          const auto offset = [&](std::size_t position, std::size_t coordinate) {
            return quantized_offset(position, coordinate, query.size(), cache->block_rows);
          };
          for (std::size_t start = 0; start < indices.size(); ++start) {
            if (visited[start] || indices[start] == start) continue;
            const auto norm = cache->quantized_norms[start];
            for (std::size_t i = 0; i < query.size(); ++i) saved[i] = cache->quantized[offset(start, i)];
            auto destination = start;
            while (indices[destination] != start) {
              const auto source = indices[destination];
              for (std::size_t i = 0; i < query.size(); ++i) {
                cache->quantized[offset(destination, i)] = cache->quantized[offset(source, i)];
              }
              cache->quantized_norms[destination] = cache->quantized_norms[source];
              visited[destination] = true;
              destination = source;
            }
            for (std::size_t i = 0; i < query.size(); ++i) cache->quantized[offset(destination, i)] = saved[i];
            cache->quantized_norms[destination] = norm;
            visited[destination] = true;
          }
        }
        if (blocks.block && !cache->equality->positions.empty()) {
          scan_blocks(first - indices.begin(), end - indices.begin(), &*filter);
        } else {
          for (auto it = first; it != end; ++it) {
            if (predicates.size() == 1 || matches_cached(*it, &*filter)) consider_cached(*it);
          }
        }
      }
    } else {
      for (const auto &entry : target.rows) {
        read_candidate(entry.first, entry.second.reference);
      }
    }
    std::sort_heap(nearest.begin(), nearest.end(), closer);
    for (const Neighbor &entry : nearest) {
      if (!cache) {
        load_row(target, entry.reference, buffer, row);
        emit();
        continue;
      }
      if (sink) {
        const float *vector = nullptr;
        projected.clear();
        for (const auto &expression : projection) {
          if (expression.query) {
            if (*expression.query == query) {
              projected.emplace_back(entry.distance);
            } else {
              if (!vector) vector = cached_vector(entry.index);
              projected.emplace_back(std::sqrt(squared_distance(vector, *expression.query)));
            }
          } else if (expression.column == vector_column) {
            if (!vector) vector = cached_vector(entry.index);
            projected.emplace_back(Vector(vector, vector + query.size()));
          } else if (expression.column == target.primary_key) {
            projected.emplace_back(entry.key);
          } else {
            const auto &values = cache->scalars[expression.column];
            if (const auto *integers = std::get_if<std::vector<std::int64_t>>(&values)) {
              projected.emplace_back((*integers)[entry.index]);
            } else {
              projected.emplace_back(std::get<std::vector<std::string>>(values)[entry.index]);
            }
          }
        }
        sink(projected);
      }
      ++result.count;
    }
    return result;
  }

  struct Candidate {
    std::int64_t key;
    RecordRef record;
    Row values;
  };
  const auto before = [&](const Candidate &left, const Candidate &right) {
    for (std::size_t i = 0; i < order.size(); ++i) {
      const int comparison = compare_values(left.values[i], right.values[i]);
      if (comparison != 0) {
        return statement.order[i].descending ? comparison > 0 : comparison < 0;
      }
    }
    return left.key < right.key;
  };
  const auto rank = [&](std::int64_t primary_key, RecordRef record) {
    Candidate candidate{primary_key, record, {}};
    candidate.values.reserve(order.size());
    for (const BoundExpression &expression : order) {
      candidate.values.push_back(evaluate(expression, row));
    }
    return candidate;
  };

  std::uint64_t maximum_candidate_size = sizeof(Candidate) + order.size() * sizeof(Value);
  for (const BoundExpression &expression : order) {
    const Column &column = target.columns[expression.column];
    if (column.type == Type::Varchar) {
      maximum_candidate_size += 4ULL * column.width;
    }
  }
  std::vector<Candidate> candidates;
  if (limit <= sort_budget / maximum_candidate_size) {
    for (const auto &entry : target.rows) {
      if (!read_match(entry.second.reference)) {
        continue;
      }
      auto candidate = rank(entry.first, entry.second.reference);
      if (candidates.size() < limit) {
        candidates.push_back(std::move(candidate));
        std::push_heap(candidates.begin(), candidates.end(), before);
      } else if (before(candidate, candidates.front())) {
        std::pop_heap(candidates.begin(), candidates.end(), before);
        candidates.back() = std::move(candidate);
        std::push_heap(candidates.begin(), candidates.end(), before);
      }
    }
    std::sort_heap(candidates.begin(), candidates.end(), before);
    for (const Candidate &candidate : candidates) {
      load_row(target, candidate.record, buffer, row);
      emit();
    }
    return result;
  }

  struct Run {
    std::uint64_t begin;
    std::uint64_t end;
  };
  std::vector<Run> runs;
  std::unique_ptr<SortFile> spill;
  std::size_t used = 0;
  const auto spill_run = [&] {
    if (candidates.empty()) return;
    if (!spill) spill = std::make_unique<SortFile>(directory_);
    std::sort(candidates.begin(), candidates.end(), before);
    const auto begin = spill->size();
    for (const auto &candidate : candidates) {
      spill->append(candidate.key, candidate.record);
    }
    runs.push_back({begin, spill->size()});
    candidates.clear();
    used = 0;
  };
  for (const auto &entry : target.rows) {
    if (!read_match(entry.second.reference)) continue;
    auto candidate = rank(entry.first, entry.second.reference);
    std::size_t bytes = sizeof(Candidate) + candidate.values.size() * sizeof(Value);
    for (const Value &value : candidate.values) {
      if (const auto *text = std::get_if<std::string>(&value)) bytes += text->capacity();
    }
    if (!candidates.empty() && (used >= sort_budget || bytes > sort_budget - used)) {
      spill_run();
    }
    candidates.push_back(std::move(candidate));
    used += bytes;
  }
  if (!spill) {
    std::sort(candidates.begin(), candidates.end(), before);
    for (const Candidate &candidate : candidates) {
      if (result.count == limit) break;
      load_row(target, candidate.record, buffer, row);
      emit();
    }
    return result;
  }
  spill_run();

  struct Cursor {
    Candidate head;
    std::uint64_t next;
    std::uint64_t end;
  };
  const auto read_ranked = [&](std::uint64_t offset) {
    const auto reference = spill->read(offset);
    load_row(target, reference.second, buffer, row);
    return rank(reference.first, reference.second);
  };
  const auto merge = [&](std::size_t begin, std::size_t end, const auto &deliver) {
    const auto after = [&](const Cursor &left, const Cursor &right) {
      return before(right.head, left.head);
    };
    std::vector<Cursor> active;
    for (std::size_t i = begin; i < end; ++i) {
      active.push_back({read_ranked(runs[i].begin), runs[i].begin + 1, runs[i].end});
    }
    std::make_heap(active.begin(), active.end(), after);
    std::uint64_t emitted = 0;
    while (!active.empty() && emitted < limit) {
      std::pop_heap(active.begin(), active.end(), after);
      Cursor cursor = std::move(active.back());
      active.pop_back();
      deliver(cursor.head);
      ++emitted;
      if (cursor.next < cursor.end && emitted < limit) {
        cursor.head = read_ranked(cursor.next++);
        active.push_back(std::move(cursor));
        std::push_heap(active.begin(), active.end(), after);
      }
    }
  };
  constexpr std::size_t merge_width = 8;
  while (runs.size() > merge_width) {
    std::vector<Run> merged;
    for (std::size_t i = 0; i < runs.size(); i += merge_width) {
      const auto begin = spill->size();
      merge(i, std::min(runs.size(), i + merge_width), [&](const Candidate &candidate) {
        spill->append(candidate.key, candidate.record);
      });
      merged.push_back({begin, spill->size()});
    }
    runs = std::move(merged);
  }
  merge(0, runs.size(), [&](const Candidate &candidate) {
    load_row(target, candidate.record, buffer, row);
    emit();
  });
  return result;
}

Execution Database::execute(std::string_view sql, const RowSink &sink, SearchMode mode,
                            const ColumnsSink &columns_sink) {
  Statement statement = parse_sql(sql);
  if (const auto *query = std::get_if<Select>(&statement)) {
    return select(*query, sink, mode, columns_sink);
  }
  if (const auto *command = std::get_if<Transaction>(&statement)) {
    transaction(*command);
    return {};
  }

  const Savepoint before = savepoint();
  Execution result;
  try {
    if (const auto *create = std::get_if<CreateTable>(&statement)) {
      result.count = create_table(*create);
    } else if (const auto *index = std::get_if<CreateVectorIndex>(&statement)) {
      result.count = create_index(*index);
    } else if (auto *new_rows = std::get_if<Insert>(&statement)) {
      result.count = insert(*new_rows);
    } else if (const auto *changed_rows = std::get_if<Update>(&statement)) {
      result.count = update(*changed_rows);
    } else {
      result.count = remove(std::get<Delete>(statement));
    }
  } catch (const SqlError &) {
    rollback(before);
    throw;
  }
  if (!transaction_) {
    commit(before);
  }
  return result;
}

}  // namespace minivector
