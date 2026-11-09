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

namespace minivector {
namespace {

enum class RecordKind : std::uint8_t { Create = 1, Put = 2, Delete = 3, Commit = 4 };

std::size_t column_index(const std::vector<Column> &columns, const std::string &name) {
  const auto found = std::find_if(columns.begin(), columns.end(), [&](const Column &column) {
    return column.name == name;
  });
  if (found == columns.end()) {
    throw SqlError("UNKNOWN_COLUMN", "unknown column " + name);
  }
  return static_cast<std::size_t>(found - columns.begin());
}

Value convert(const Column &column, const Value &value) {
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
      Vector vector;
      if (const auto *components = std::get_if<Vector>(&value)) {
        vector = *components;
      } else if (const auto *text = std::get_if<std::string>(&value)) {
        vector = parse_vector(*text);
      } else {
        break;
      }
      if (vector.size() != column.width) {
        throw SqlError("VECTOR_DIMENSION", "vector width does not match " + column.name);
      }
      return vector;
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

bool matches(const Row &row, const std::vector<BoundPredicate> &predicates) {
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
  std::optional<Vector> query;
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
  return {index, expression.query, expression.alias.empty() ? default_name : expression.alias};
}

double squared_distance(const Vector &left, const Vector &right) {
  double sum = 0;
  for (std::size_t i = 0; i < left.size(); ++i) {
    const double difference = static_cast<double>(left[i]) - static_cast<double>(right[i]);
    sum += difference * difference;
  }
  return sum;
}

Value evaluate(const BoundExpression &expression, const Row &row, bool order_key) {
  const Value &value = row[expression.column];
  if (!expression.query) {
    return value;
  }
  const double squared = squared_distance(std::get<Vector>(value), *expression.query);
  return order_key ? squared : std::sqrt(squared);
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

void Database::rollback(Savepoint point) {
  while (undo_.size() > point.undo_count) {
    const Undo undo = undo_.back();
    undo_.pop_back();
    Table &target = *tables_[undo.table];
    if (undo.created_table) {
      names_.erase(target.name);
      tables_.pop_back();
    } else if (undo.previous) {
      target.rows.insert_or_assign(undo.key, *undo.previous);
    } else {
      target.rows.erase(undo.key);
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
    if (apply_record(payload, record)) {
      committed = next;
    }
  }
  if (committed != journal_.size()) {
    rollback({committed, 0});
    journal_.sync();
  }
}

bool Database::apply_record(const Bytes &payload, RecordRef record) {
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
    tables_.push_back(std::make_unique<Table>(Table{id, std::move(name), std::move(columns), primary, {}}));
    undo_.push_back({id, true, 0, std::nullopt});
    return false;
  }
  if (id >= tables_.size() || (kind != RecordKind::Put && kind != RecordKind::Delete)) {
    throw std::runtime_error("invalid mutation in journal");
  }
  const std::int64_t key = input.i64();
  Table &target = *tables_[id];
  const auto existing = target.rows.find(key);
  std::optional<RecordRef> previous;
  if (existing != target.rows.end()) {
    previous = existing->second;
  }
  undo_.push_back({id, false, key, previous});
  if (kind == RecordKind::Put) {
    target.rows.insert_or_assign(key, record);
  } else {
    input.finish();
    target.rows.erase(key);
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
      for (float &value : vector) {
        value = input.f32();
      }
    }
  }
  input.finish();
}

void Database::put_row(const Table &table, std::int64_t key, const Row &row) {
  Bytes payload{static_cast<std::uint8_t>(RecordKind::Put)};
  put_u32(payload, table.id);
  put_i64(payload, key);
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i == table.primary_key) {
      continue;
    }
    switch (table.columns[i].type) {
      case Type::Bigint: put_i64(payload, std::get<std::int64_t>(row[i])); break;
      case Type::Varchar: put_string(payload, std::get<std::string>(row[i])); break;
      case Type::Vector:
        for (const float value : std::get<Vector>(row[i])) {
          put_float(payload, value);
        }
        break;
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

std::uint64_t Database::insert(const Insert &statement) {
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
  for (const Row &row : statement.rows) {
    if (row.size() != positions.size()) {
      throw SqlError("COLUMN_COUNT", "INSERT value count does not match the table");
    }
    for (std::size_t i = 0; i < positions.size(); ++i) {
      values[positions[i]] = convert(target.columns[positions[i]], row[i]);
    }
    const std::int64_t key = std::get<std::int64_t>(values[target.primary_key]);
    if (target.rows.count(key) != 0) {
      throw SqlError("DUPLICATE_KEY", "duplicate primary key " + std::to_string(key));
    }
    put_row(target, key, values);
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
  Row row;
  std::uint64_t count = 0;
  while (next != target.rows.end()) {
    const auto current = next++;
    load_row(target, current->second, buffer, row);
    if (matches(row, predicates)) {
      for (const auto &change : changes) {
        row[change.first] = change.second;
      }
      put_row(target, current->first, row);
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
      load_row(target, current->second, buffer, row);
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

Execution Database::select(const Select &statement, const RowSink &sink) {
  Table &target = table(statement.table);
  const auto predicates = bind_predicates(target.columns, statement.predicates);
  std::vector<BoundExpression> projection;
  if (statement.all_columns) {
    for (std::size_t i = 0; i < target.columns.size(); ++i) {
      projection.push_back({i, std::nullopt, target.columns[i].name});
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
        projected.push_back(evaluate(expression, row, false));
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
    if (found != target.rows.end() && read_match(found->second)) {
      emit();
    }
    return result;
  }
  if (in_key_order) {
    const auto scan = [&](auto current, auto end) {
      for (; current != end && result.count < limit; ++current) {
        if (read_match(current->second)) {
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
      candidate.values.push_back(evaluate(expression, row, true));
    }
    return candidate;
  };

  constexpr std::size_t sort_budget = 2 * 1024 * 1024;
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
      if (!read_match(entry.second)) {
        continue;
      }
      auto candidate = rank(entry.first, entry.second);
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
    if (!read_match(entry.second)) continue;
    auto candidate = rank(entry.first, entry.second);
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

Execution Database::execute(std::string_view sql, const RowSink &sink) {
  const Statement statement = parse_sql(sql);
  if (const auto *query = std::get_if<Select>(&statement)) {
    return select(*query, sink);
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
    } else if (const auto *new_rows = std::get_if<Insert>(&statement)) {
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
