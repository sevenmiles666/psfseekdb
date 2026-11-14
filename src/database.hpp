#pragma once

#include "journal.hpp"
#include "sql.hpp"

#include <functional>
#include <map>
#include <list>
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
    std::size_t cache_reads = 0;
    bool delay_cache = false;
    bool cache_unfit = false;
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

  struct CachedEquality {
    std::size_t column;
    std::vector<std::uint32_t> rows;
    std::vector<std::uint32_t> positions;
  };

  using CachedColumn = std::variant<std::monostate, std::vector<std::int64_t>, std::vector<std::string>>;

  struct SearchCache {
    std::uint32_t table;
    std::size_t vector_column = 0;
    std::uint64_t used_bytes = 0;
    std::vector<bool> unavailable_columns;
    std::size_t switch_column = 0;
    std::uint64_t switch_credit = 0;
    std::vector<std::int64_t> keys;
    Vector vectors;
    std::vector<CachedColumn> scalars;
    std::optional<CachedEquality> equality;
    double quantization_step;
    double quantization_error;
    std::vector<std::int8_t> quantized;
    std::vector<std::int32_t> quantized_norms;
    std::vector<std::uint8_t> negative_zeros;
    bool exact_codes = false;
    bool can_group_quantized = false;
    std::size_t block_rows;
  };

  void recover();
  void maybe_checkpoint();
  void checkpoint();
  bool apply_record(const Bytes &payload, RecordRef record, bool replay = false);
  void write_change(const Bytes &payload);
  Savepoint savepoint() const;
  void rollback(Savepoint point, bool replay = false);
  void commit(Savepoint point);
  void transaction(Transaction command);

  Table &table(const std::string &name);
  float read_projection(const Table &table, Reader &row, VectorEncoding encoding) const;
  void rebuild_index(Table &table);
  void set_row(Table &table, std::int64_t key, std::optional<StoredRow> row, bool replay);
  void invalidate_cache(std::uint32_t table);
  SearchCache *search_cache(Table &table, std::size_t vector_column,
                            const std::vector<std::size_t> &columns);
  void cache_columns(SearchCache &cache, const Table &table, const std::vector<std::size_t> &columns);
  void reorder_quantized(SearchCache &cache, const std::vector<std::uint32_t> &permutation,
                         std::size_t dimension);
  std::uint64_t cache_bytes() const;
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
  std::list<SearchCache> search_caches_;
  std::uint64_t checkpoint_check_at_ = 4 * 1024 * 1024;
};

}  // namespace minivector
