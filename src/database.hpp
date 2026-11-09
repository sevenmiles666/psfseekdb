#pragma once

#include "journal.hpp"
#include "sql.hpp"

#include <functional>
#include <map>
#include <memory>

namespace minivector {

using RowSink = std::function<void(const Row &)>;

struct Execution {
  std::uint64_t count = 0;
  std::vector<std::string> columns;
};

class Database {
public:
  explicit Database(const std::filesystem::path &directory);

  Execution execute(std::string_view sql, const RowSink &sink = {});
  void close();

private:
  struct Table {
    std::uint32_t id;
    std::string name;
    std::vector<Column> columns;
    std::size_t primary_key;
    std::map<std::int64_t, RecordRef> rows;
  };

  struct Savepoint {
    std::uint64_t offset;
    std::size_t undo_count;
  };

  struct Undo {
    std::uint32_t table;
    bool created_table;
    std::int64_t key;
    std::optional<RecordRef> previous;
  };

  void recover();
  bool apply_record(const Bytes &payload, RecordRef record);
  void write_change(const Bytes &payload);
  Savepoint savepoint() const;
  void rollback(Savepoint point);
  void commit(Savepoint point);
  void transaction(Transaction command);

  Table &table(const std::string &name);
  void load_row(const Table &table, RecordRef record, Bytes &buffer, Row &row);
  void put_row(const Table &table, std::int64_t key, const Row &row);
  std::uint64_t create_table(const CreateTable &statement);
  std::uint64_t insert(const Insert &statement);
  std::uint64_t update(const Update &statement);
  std::uint64_t remove(const Delete &statement);
  Execution select(const Select &statement, const RowSink &sink);

  std::filesystem::path directory_;
  Journal journal_;
  std::map<std::string, std::uint32_t> names_;
  std::vector<std::unique_ptr<Table>> tables_;
  std::vector<Undo> undo_;
  std::optional<Savepoint> transaction_;
};

}  // namespace minivector
