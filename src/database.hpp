#pragma once

#include "journal.hpp"
#include "sql.hpp"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace minivector {

using RowSink = std::function<void(const Row &)>;
using ColumnsSink = std::function<void(const std::vector<std::string> &)>;

enum class SearchMode { Exact, Ann };

struct Execution {
  std::uint64_t count = 0;
  std::vector<std::string> columns;
};

class Database {
public:
  explicit Database(const std::filesystem::path &directory);

  Execution execute(std::string_view sql, const RowSink &sink = {},
                    SearchMode mode = SearchMode::Exact, const ColumnsSink &columns_sink = {});
  void close();

private:
  struct StoredRow {
    RecordRef reference;
    float projection = 0;
  };

  struct VectorIndex {
    std::string name;
    std::size_t column;
    std::set<std::pair<float, std::int64_t>> ordered;
  };

  struct Table {
    std::uint32_t id;
    std::string name;
    std::vector<Column> columns;
    std::size_t primary_key;
    std::map<std::int64_t, StoredRow> rows;
    std::optional<VectorIndex> index;
  };

  struct Savepoint {
    std::uint64_t offset;
    std::size_t undo_count;
  };

  enum class UndoKind { Table, Index, Row };

  struct Undo {
    std::uint32_t table;
    UndoKind kind;
    std::int64_t key;
    std::optional<StoredRow> previous;
  };

  struct CachedRow {
    std::int64_t key;
    RecordRef reference;
  };

  struct CachedEquality {
    std::size_t column;
    std::vector<std::uint32_t> rows;
  };

  struct SearchCache {
    std::uint32_t table;
    std::vector<CachedRow> rows;
    Vector vectors;
    std::vector<Value> scalars;
    std::optional<CachedEquality> equality;
    double quantization_step;
    std::vector<std::int8_t> quantized;
  };

  void recover();
  bool apply_record(const Bytes &payload, RecordRef record, bool replay = false);
  void write_change(const Bytes &payload);
  Savepoint savepoint() const;
  void rollback(Savepoint point, bool replay = false);
  void commit(Savepoint point);
  void transaction(Transaction command);

  Table &table(const std::string &name);
  float read_projection(const Table &table, Reader &row) const;
  void rebuild_index(Table &table);
  void set_row(Table &table, std::int64_t key, std::optional<StoredRow> row, bool replay);
  void invalidate_cache(std::uint32_t table);
  SearchCache *search_cache(const Table &table, std::size_t vector_column);
  void load_row(const Table &table, RecordRef record, Bytes &buffer, Row &row);
  void put_row(const Table &table, std::int64_t key, const Row &row, Bytes &payload);
  std::uint64_t create_table(const CreateTable &statement);
  std::uint64_t create_index(const CreateVectorIndex &statement);
  std::uint64_t insert(Insert &statement);
  std::uint64_t update(const Update &statement);
  std::uint64_t remove(const Delete &statement);
  Execution select(const Select &statement, const RowSink &sink, SearchMode mode,
                   const ColumnsSink &columns_sink);

  std::filesystem::path directory_;
  Journal journal_;
  std::map<std::string, std::uint32_t> names_;
  std::vector<std::unique_ptr<Table>> tables_;
  std::vector<Undo> undo_;
  std::optional<Savepoint> transaction_;
  std::optional<SearchCache> search_cache_;
  std::optional<std::uint32_t> oversized_cache_;
};

}  // namespace minivector
