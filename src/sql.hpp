#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace minivector {

class SqlError : public std::runtime_error {
public:
  SqlError(std::string code, const std::string &message);
  const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

using Vector = std::vector<float>;
using Value = std::variant<std::int64_t, std::string, Vector, double>;
using Row = std::vector<Value>;

enum class Type : std::uint8_t { Bigint = 1, Varchar = 2, Vector = 3 };

struct Column {
  std::string name;
  Type type;
  std::uint32_t width = 0;
};

enum class Comparison { Eq, Lt, Le, Gt, Ge };

struct Predicate {
  std::string column;
  Comparison comparison;
  Value value;
};

struct Expression {
  std::string column;
  std::optional<Vector> query;
  std::string alias;
};

struct Order {
  Expression expression;
  bool descending;
};

struct CreateTable {
  std::string table;
  std::vector<Column> columns;
  std::size_t primary_key;
};

struct CreateVectorIndex {
  std::string name;
  std::string table;
  std::string column;
};

struct Insert {
  std::string table;
  std::vector<std::string> columns;
  std::vector<Row> rows;
};

struct Assignment {
  std::string column;
  Value value;
};

struct Update {
  std::string table;
  std::vector<Assignment> assignments;
  std::vector<Predicate> predicates;
};

struct Delete {
  std::string table;
  std::vector<Predicate> predicates;
};

struct Select {
  std::string table;
  bool all_columns = false;
  std::vector<Expression> columns;
  std::vector<Predicate> predicates;
  std::vector<Order> order;
  std::optional<std::uint64_t> limit;
};

enum class Transaction { Begin, Commit, Rollback };

using Statement = std::variant<CreateTable, CreateVectorIndex, Insert, Update, Delete, Select,
                               Transaction>;

Statement parse_sql(std::string_view sql);
Vector parse_vector(std::string_view text);
std::size_t unicode_length(std::string_view text);

}  // namespace minivector
