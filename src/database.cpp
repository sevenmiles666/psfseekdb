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

enum class RecordKind : std::uint8_t { Create = 1, Put = 2, Delete = 3, Commit = 4, Index = 5 };

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

bool matches(const Value *row, const std::vector<BoundPredicate> &predicates) {
  for (const BoundPredicate &predicate : predicates) {
    const int order = compare_values(row[predicate.column], predicate.value);
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
  return matches(row.data(), predicates);
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
std::uint32_t quantized_squared_distance(const std::int8_t *left, const std::int8_t *right, std::size_t size) {
  auto low = _mm256_setzero_si256();
  auto high = _mm256_setzero_si256();
  std::size_t i = 0;
  for (; i + 32 <= size; i += 32) {
    const auto left0 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(left + i)));
    const auto right0 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(right + i)));
    const auto left1 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(left + i + 16)));
    const auto right1 = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(right + i + 16)));
    const auto diff0 = _mm256_sub_epi16(left0, right0);
    const auto diff1 = _mm256_sub_epi16(left1, right1);
    low = _mm256_add_epi32(low, _mm256_madd_epi16(diff0, diff0));
    high = _mm256_add_epi32(high, _mm256_madd_epi16(diff1, diff1));
  }
  auto lanes = _mm256_add_epi32(low, high);
  for (; i + 16 <= size; i += 16) {
    const auto a = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(left + i)));
    const auto b = _mm256_cvtepi8_epi16(_mm_loadu_si128(reinterpret_cast<const __m128i *>(right + i)));
    const auto diff = _mm256_sub_epi16(a, b);
    lanes = _mm256_add_epi32(lanes, _mm256_madd_epi16(diff, diff));
  }
  auto sum = _mm_add_epi32(_mm256_castsi256_si128(lanes), _mm256_extracti128_si256(lanes, 1));
  sum = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum, sum);
  std::uint32_t result = _mm_cvtsi128_si32(sum);
  for (; i < size; ++i) {
    const int diff = int(left[i]) - int(right[i]);
    result += diff * diff;
  }
  return result;
}
#endif

using QuantizedKernel = std::uint32_t (*)(const std::int8_t *, const std::int8_t *, std::size_t);

QuantizedKernel integer_distance_kernel() {
#if defined(__x86_64__) && defined(__GNUC__)
  return __builtin_cpu_supports("avx2") ? quantized_squared_distance : nullptr;
#else
  return nullptr;
#endif
}

std::int8_t quantize_component(float value, double step) {
  return static_cast<std::int8_t>(std::lround(static_cast<double>(value) / step));
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
  if (kind != RecordKind::Put && kind != RecordKind::Delete) {
    throw std::runtime_error("invalid mutation in journal");
  }
  const std::int64_t key = input.i64();
  const auto existing = target.rows.find(key);
  std::optional<StoredRow> previous;
  if (existing != target.rows.end()) {
    previous = existing->second;
  }
  undo_.push_back({id, UndoKind::Row, key, previous});
  if (kind == RecordKind::Put) {
    const float projection = target.index && !replay ? read_projection(target, input) : 0;
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

float Database::read_projection(const Table &table, Reader &row) const {
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
  const float projection = row.f32();
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
    if (input.byte() != static_cast<std::uint8_t>(RecordKind::Put) || input.u32() != table.id ||
        input.i64() != row.first) {
      throw std::runtime_error("row reference does not match vector index");
    }
    row.second.projection = read_projection(table, input);
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
  const std::uint64_t fixed_per_row = sizeof(CachedRow) + dimension * sizeof(float) +
                                     table.columns.size() * sizeof(Value) + sizeof(std::uint32_t);
  const auto count = table.rows.size();
  if (count > budget / fixed_per_row) {
    oversized_cache_ = table.id;
    return nullptr;
  }

  search_cache_.reset();
  SearchCache cache{table.id, {}, {}, {}, std::nullopt, 0.0, {}};
  cache.rows.reserve(count);
  cache.vectors.reserve(count * dimension);
  cache.scalars.reserve(count * table.columns.size());
  std::uint64_t used = count * fixed_per_row;
  Bytes buffer;
  Row row;
  for (const auto &entry : table.rows) {
    load_row(table, entry.second.reference, buffer, row);
    const auto &vector = std::get<Vector>(row[vector_column]);
    cache.vectors.insert(cache.vectors.end(), vector.begin(), vector.end());
    for (std::size_t column = 0; column < row.size(); ++column) {
      if (column == vector_column) {
        cache.scalars.emplace_back(std::int64_t{0});
        continue;
      }
      if (const auto *text = std::get_if<std::string>(&row[column])) {
        const std::uint64_t bytes = text->size() + 1;
        if (bytes > budget - used) {
          oversized_cache_ = table.id;
          return nullptr;
        }
        used += bytes;
      }
      cache.scalars.push_back(row[column]);
    }
    cache.rows.push_back({entry.first, entry.second.reference});
  }
  if (dimension >= 32 && dimension <= 4096 && cache.vectors.size() >= 256 * 1024 &&
      cache.vectors.size() <= budget - used && integer_distance_kernel()) {
    float max_abs = 0;
    for (const float value : cache.vectors) max_abs = std::max(max_abs, std::abs(value));
    if (max_abs != 0) {
      cache.quantization_step = static_cast<double>(max_abs) / 127.0;
      cache.quantized.reserve(cache.vectors.size());
      for (const float value : cache.vectors) {
        cache.quantized.push_back(quantize_component(value, cache.quantization_step));
      }
    }
  }
  search_cache_ = std::move(cache);
  return &*search_cache_;
}

void Database::load_row(const Table &table, RecordRef record, Bytes &buffer, Row &row) {
  journal_.read(record, buffer);
  Reader input(buffer);
  if (input.byte() != static_cast<std::uint8_t>(RecordKind::Put) || input.u32() != table.id) {
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
      if (column.width > input.remaining() / sizeof(float)) {
        throw std::runtime_error("incomplete vector in journal");
      }
      if (!std::holds_alternative<Vector>(row[i])) {
        row[i] = Vector{};
      }
      auto &vector = std::get<Vector>(row[i]);
      vector.resize(column.width);
      input.floats(vector.data(), vector.size());
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
        put_floats(payload, vector.data(), vector.size());
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
  if (order.size() == 1 && order.front().query && !statement.order.front().descending &&
      limit <= sort_budget / sizeof(Neighbor)) {
    const auto vector_column = order.front().column;
    const Vector &query = *order.front().query;
    const auto closer = [](const Neighbor &left, const Neighbor &right) {
      if (left.distance != right.distance) {
        return left.distance < right.distance;
      }
      return left.key < right.key;
    };
    auto *cache = search_cache(target, vector_column);
    const auto estimate = distance_estimator(query.size());
    QuantizedKernel integer_distance = nullptr;
    std::vector<std::int8_t> query_quantized;
    if (cache && !cache->quantized.empty() &&
        std::all_of(query.begin(), query.end(), [&](float value) {
          return std::abs(value) <= 127.0 * cache->quantization_step;
        })) {
      integer_distance = integer_distance_kernel();
      query_quantized.reserve(query.size());
      for (const float value : query) {
        query_quantized.push_back(quantize_component(value, cache->quantization_step));
      }
    }
    double cutoff = std::numeric_limits<double>::infinity();
    double worst_squared = cutoff;
    std::vector<Neighbor> nearest;
    nearest.reserve(std::min<std::uint64_t>(limit, target.rows.size()));
    const auto consider = [&](std::int64_t key, RecordRef reference, const float *vector,
                              std::size_t index = 0) {
      Neighbor candidate{std::sqrt(squared_distance(vector, query)), key, reference, index};
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
        if (integer_distance) {
          // Each rounded coordinate contributes at most half a step of error.
          // Triangle inequality adds one step per coordinate for the two vectors.
          const double radius = worst_distance / cache->quantization_step *
                                (1.0 + (4.0 * query.size() + 4.0) * std::numeric_limits<double>::epsilon()) +
                                std::sqrt(static_cast<double>(query.size())) * 1.00000001;
          cutoff = radius * radius;
        } else {
          cutoff = worst_squared * estimate.relative_margin + estimate.absolute_margin;
        }
      }
    };
    const auto read_candidate = [&](std::int64_t key, RecordRef reference) {
      if (read_match(reference)) {
        consider(key, reference, std::get<Vector>(row[vector_column]).data());
      }
    };

    const auto consider_cached = [&](std::size_t index) {
      if (!matches(cache->scalars.data() + index * target.columns.size(), predicates)) return;
      const auto &entry = cache->rows[index];
      const auto *vector = cache->vectors.data() + index * query.size();
      if (nearest.size() == limit) {
        if (integer_distance) {
          if (integer_distance(cache->quantized.data() + index * query.size(),
                               query_quantized.data(), query.size()) > cutoff) return;
        } else if (estimate.kernel && estimate.kernel(vector, query.data(), query.size()) > cutoff) {
          return;
        }
      }
      consider(entry.key, entry.reference, vector, index);
    };

    if (mode == SearchMode::Ann && target.index) {
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
          const auto found = std::lower_bound(cache->rows.begin(), cache->rows.end(), entry.second,
              [](const CachedRow &row, std::int64_t key) { return row.key < key; });
          if (found == cache->rows.end() || found->key != entry.second) {
            throw std::runtime_error("vector index references missing row");
          }
          consider_cached(static_cast<std::size_t>(found - cache->rows.begin()));
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
        for (std::size_t i = 0; i < cache->rows.size(); ++i) {
          consider_cached(i);
        }
      } else {
        const auto scalar = [&](std::uint32_t i) -> const Value & {
          return cache->scalars[std::size_t(i) * target.columns.size() + filter->column];
        };
        if (!cache->equality) {
          CachedEquality index{filter->column, {}};
          index.rows.resize(cache->rows.size());
          std::iota(index.rows.begin(), index.rows.end(), std::uint32_t{0});
          std::sort(index.rows.begin(), index.rows.end(), [&](auto left, auto right) {
            const int comparison = compare_values(scalar(left), scalar(right));
            return comparison == 0 ? left < right : comparison < 0;
          });
          cache->equality.emplace(std::move(index));
        }
        const auto &indices = cache->equality->rows;
        const auto first = std::lower_bound(indices.begin(), indices.end(), filter->value,
            [&](auto index, const Value &value) { return compare_values(scalar(index), value) < 0; });
        const auto end = std::upper_bound(first, indices.end(), filter->value,
            [&](const Value &value, auto index) { return compare_values(value, scalar(index)) < 0; });
        for (auto it = first; it != end; ++it) {
          consider_cached(*it);
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
        const auto *vector = cache->vectors.data() + entry.index * query.size();
        const auto *scalars = cache->scalars.data() + entry.index * target.columns.size();
        projected.clear();
        for (const auto &expression : projection) {
          if (expression.query) {
            projected.emplace_back(std::sqrt(squared_distance(vector, *expression.query)));
          } else if (expression.column == vector_column) {
            projected.emplace_back(Vector(vector, vector + query.size()));
          } else {
            projected.push_back(scalars[expression.column]);
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
