#include "sql.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>

namespace minivector {

SqlError::SqlError(std::string code, const std::string &message)
    : std::runtime_error(message), code_(std::move(code)) {}

std::size_t unicode_length(std::string_view text) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++count) {
    const unsigned char first = text[i++];
    if (first < 0x80) {
      continue;
    }
    std::uint32_t codepoint = 0;
    unsigned int continuation = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      codepoint = first & 0x1f;
      continuation = 1;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      codepoint = first & 0x0f;
      continuation = 2;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      codepoint = first & 0x07;
      continuation = 3;
      minimum = 0x10000;
    } else {
      throw SqlError("INVALID_UTF8", "invalid UTF-8 lead byte");
    }
    if (continuation > text.size() - i) {
      throw SqlError("INVALID_UTF8", "incomplete UTF-8 character");
    }
    for (unsigned int j = 0; j < continuation; ++j) {
      const unsigned char byte = text[i++];
      if ((byte & 0xc0) != 0x80) {
        throw SqlError("INVALID_UTF8", "invalid UTF-8 continuation byte");
      }
      codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      throw SqlError("INVALID_UTF8", "invalid Unicode scalar value");
    }
  }
  return count;
}

namespace {

enum class TokenKind { End, Word, Identifier, Text, Number, Symbol };

struct Token {
  TokenKind kind;
  std::string text;
};

bool is_digit(char value) { return value >= '0' && value <= '9'; }

bool is_letter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

bool is_space(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' || value == '\f';
}

std::string lowercase(std::string value) {
  for (char &ch : value) {
    if (ch >= 'A' && ch <= 'Z') {
      ch = static_cast<char>(ch - 'A' + 'a');
    }
  }
  return value;
}

class Lexer {
public:
  explicit Lexer(std::string_view input) : input_(input) {}

  Token next() {
    while (is_space(peek())) {
      ++position_;
    }
    if (position_ == input_.size()) {
      return {TokenKind::End, {}};
    }
    const std::size_t start = position_;
    const char first = input_[position_++];
    if (is_letter(first)) {
      while (is_letter(peek()) || is_digit(peek())) {
        ++position_;
      }
      return {TokenKind::Word, lowercase(slice(start))};
    }
    if (first == '\'' || first == '"' || first == '`') {
      return quoted(first);
    }
    if (is_digit(first) || first == '+' || first == '-' || first == '.') {
      if (first != '.') {
        while (is_digit(peek())) {
          ++position_;
        }
        if (peek() == '.') {
          ++position_;
        }
      }
      while (is_digit(peek())) {
        ++position_;
      }
      if (peek() == 'e' || peek() == 'E') {
        ++position_;
        if (peek() == '+' || peek() == '-') {
          ++position_;
        }
        while (is_digit(peek())) {
          ++position_;
        }
      }
      return {TokenKind::Number, slice(start)};
    }
    if (std::string_view("(),[];*=<>").find(first) != std::string_view::npos) {
      if ((first == '<' || first == '>') && peek() == '=') {
        ++position_;
      }
      return {TokenKind::Symbol, slice(start)};
    }
    throw SqlError("SYNTAX", "unexpected character at byte " + std::to_string(start));
  }

private:
  char peek() const { return position_ < input_.size() ? input_[position_] : '\0'; }

  std::string slice(std::size_t start) const {
    return std::string(input_.substr(start, position_ - start));
  }

  Token quoted(char delimiter) {
    std::string value;
    while (position_ < input_.size()) {
      char ch = input_[position_++];
      if (ch == delimiter) {
        if (peek() == delimiter) {
          ++position_;
        } else {
          unicode_length(value);
          if (delimiter == '`') {
            if (value.empty() || value.find('\0') != std::string::npos) {
              throw SqlError("SYNTAX", "invalid quoted identifier");
            }
            return {TokenKind::Identifier, lowercase(std::move(value))};
          }
          return {TokenKind::Text, std::move(value)};
        }
      } else if (ch == '\\' && delimiter != '`') {
        if (position_ == input_.size()) {
          throw SqlError("SYNTAX", "incomplete string escape");
        }
        switch (input_[position_++]) {
          case '\\': ch = '\\'; break;
          case '\'': ch = '\''; break;
          case '"': ch = '"'; break;
          case '0': ch = '\0'; break;
          case 'n': ch = '\n'; break;
          case 'r': ch = '\r'; break;
          case 't': ch = '\t'; break;
          case 'b': ch = '\b'; break;
          default: throw SqlError("SYNTAX", "unsupported string escape");
        }
      }
      value.push_back(ch);
    }
    throw SqlError("SYNTAX", "unterminated quoted value");
  }

  std::string_view input_;
  std::size_t position_ = 0;
};

template <class Integer>
Integer read_integer(std::string_view text) {
  if (!text.empty() && text.front() == '+') {
    text.remove_prefix(1);
    if (!text.empty() && text.front() == '-') {
      throw SqlError("INVALID_NUMBER", "invalid integer sign");
    }
  }
  Integer result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
    throw SqlError("INVALID_NUMBER", "integer is invalid or out of range");
  }
  return result;
}

class Parser {
public:
  explicit Parser(std::string_view sql) : lexer_(sql), token_(lexer_.next()) {}

  Statement statement() {
    Statement result;
    if (take("create")) {
      if (take("table")) {
        result = create_table();
      } else if (take("vector")) {
        expect("index");
        CreateVectorIndex index{identifier(), {}, {}};
        expect("on");
        index.table = identifier();
        expect("(");
        index.column = identifier();
        expect(")");
        result = std::move(index);
      } else {
        throw SqlError("UNSUPPORTED", "expected CREATE TABLE or CREATE VECTOR INDEX");
      }
    } else if (take("insert")) {
      result = insert();
    } else if (take("select")) {
      result = select();
    } else if (take("update")) {
      result = update();
    } else if (take("delete")) {
      result = remove();
    } else if (take("begin")) {
      result = Transaction::Begin;
    } else if (take("commit")) {
      result = Transaction::Commit;
    } else if (take("rollback")) {
      result = Transaction::Rollback;
    } else {
      throw SqlError("UNSUPPORTED", "unsupported SQL statement");
    }
    take(";");
    finish();
    return result;
  }

  Vector vector() {
    expect("[");
    Vector result;
    if (!take("]")) {
      do {
        const std::string number = consume(TokenKind::Number, "vector component");
        const char *last = number.data() + number.size();
        const char *first = number.data() + (number.front() == '+' ? 1 : 0);
        float value = 0;
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc{} || parsed.ptr != last) {
          // Keep strtof rounding for components below the smallest float32 subnormal.
          char *end = nullptr;
          value = std::strtof(number.c_str(), &end);
          if (end != last || !std::isfinite(value)) {
            throw SqlError("INVALID_VECTOR", "vector component is not finite float32");
          }
        }
        result.push_back(value);
      } while (take(","));
      expect("]");
    }
    return result;
  }

  void finish() const {
    if (token_.kind != TokenKind::End) {
      throw SqlError("SYNTAX", "unexpected trailing SQL tokens");
    }
  }

private:
  void advance() { token_ = lexer_.next(); }

  bool take(std::string_view text) {
    if ((token_.kind == TokenKind::Word || token_.kind == TokenKind::Symbol) &&
        token_.text == text) {
      advance();
      return true;
    }
    return false;
  }

  void expect(std::string_view text) {
    if (!take(text)) {
      throw SqlError("SYNTAX", "expected " + std::string(text));
    }
  }

  std::string consume(TokenKind kind, std::string_view description) {
    if (token_.kind != kind) {
      throw SqlError("SYNTAX", "expected " + std::string(description));
    }
    std::string value = std::move(token_.text);
    advance();
    return value;
  }

  std::string identifier() {
    if (token_.kind == TokenKind::Identifier) {
      return consume(TokenKind::Identifier, "identifier");
    }
    return consume(TokenKind::Word, "identifier");
  }

  Value value() {
    if (token_.kind == TokenKind::Number) {
      return read_integer<std::int64_t>(consume(TokenKind::Number, "integer"));
    }
    if (token_.kind == TokenKind::Text) {
      return consume(TokenKind::Text, "string");
    }
    if (token_.kind == TokenKind::Symbol && token_.text == "[") {
      return vector();
    }
    throw SqlError("SYNTAX", "expected a BIGINT, VARCHAR or vector literal");
  }

  CreateTable create_table() {
    CreateTable result{identifier(), {}, 0};
    std::optional<std::string> primary;
    const auto mark_primary = [&](std::string name) {
      if (primary) {
        throw SqlError("INVALID_SCHEMA", "exactly one primary key is required");
      }
      primary = std::move(name);
    };
    std::set<std::string> names;
    bool has_vector = false;
    expect("(");
    do {
      if (take("primary")) {
        expect("key");
        expect("(");
        mark_primary(identifier());
        expect(")");
        continue;
      }
      Column column{identifier(), Type::Bigint, 0};
      if (!names.insert(column.name).second) {
        throw SqlError("INVALID_SCHEMA", "duplicate column " + column.name);
      }
      if (take("bigint")) {
        column.type = Type::Bigint;
      } else {
        if (take("varchar")) {
          column.type = Type::Varchar;
        } else if (take("vector")) {
          if (has_vector) {
            throw SqlError("INVALID_SCHEMA", "a table may have one vector column");
          }
          column.type = Type::Vector;
          has_vector = true;
        } else {
          throw SqlError("UNSUPPORTED", "unsupported column type");
        }
        expect("(");
        column.width = read_integer<std::uint32_t>(consume(TokenKind::Number, "column width"));
        if (column.width == 0) {
          throw SqlError("INVALID_SCHEMA", "column width must be positive");
        }
        expect(")");
      }
      if (take("primary")) {
        expect("key");
        mark_primary(column.name);
      }
      result.columns.push_back(std::move(column));
    } while (take(","));
    expect(")");
    if (!primary) {
      throw SqlError("INVALID_SCHEMA", "a BIGINT primary key is required");
    }
    for (std::size_t i = 0; i < result.columns.size(); ++i) {
      if (result.columns[i].name == *primary && result.columns[i].type == Type::Bigint) {
        result.primary_key = i;
        return result;
      }
    }
    throw SqlError("INVALID_SCHEMA", "primary key must name a BIGINT column");
  }

  Insert insert() {
    expect("into");
    Insert result{identifier(), {}, {}};
    if (take("(")) {
      do {
        result.columns.push_back(identifier());
      } while (take(","));
      expect(")");
    }
    expect("values");
    do {
      Row row;
      expect("(");
      do {
        row.push_back(value());
      } while (take(","));
      expect(")");
      result.rows.push_back(std::move(row));
    } while (take(","));
    return result;
  }

  Comparison comparison() {
    if (take("=")) return Comparison::Eq;
    if (take("<=")) return Comparison::Le;
    if (take("<")) return Comparison::Lt;
    if (take(">=")) return Comparison::Ge;
    if (take(">")) return Comparison::Gt;
    throw SqlError("SYNTAX", "expected a supported comparison operator");
  }

  void conjunction(std::vector<Predicate> &result, unsigned int depth) {
    if (depth > 64) {
      throw SqlError("SYNTAX", "WHERE nesting is too deep");
    }
    do {
      if (take("(")) {
        conjunction(result, depth + 1);
        expect(")");
      } else {
        result.push_back({identifier(), comparison(), value()});
      }
    } while (take("and"));
  }

  std::vector<Predicate> where() {
    std::vector<Predicate> result;
    if (take("where")) {
      conjunction(result, 0);
    }
    return result;
  }

  Update update() {
    Update result{identifier(), {}, {}};
    expect("set");
    do {
      std::string name = identifier();
      expect("=");
      result.assignments.push_back({std::move(name), value()});
    } while (take(","));
    result.predicates = where();
    return result;
  }

  Delete remove() {
    expect("from");
    return {identifier(), where()};
  }

  Expression expression() {
    const bool distance_name = token_.kind == TokenKind::Word && token_.text == "l2_distance";
    std::string name = identifier();
    if (distance_name && take("(")) {
      std::string column = identifier();
      expect(",");
      Vector query;
      if (token_.kind == TokenKind::Text) {
        query = parse_vector(consume(TokenKind::Text, "vector"));
      } else {
        query = vector();
      }
      expect(")");
      return {std::move(column), std::move(query), {}};
    }
    return {std::move(name), std::nullopt, {}};
  }

  Select select() {
    Select result;
    result.all_columns = take("*");
    if (!result.all_columns) {
      do {
        Expression column = expression();
        if (take("as")) {
          column.alias = identifier();
        }
        result.columns.push_back(std::move(column));
      } while (take(","));
    }
    expect("from");
    result.table = identifier();
    result.predicates = where();
    if (take("order")) {
      expect("by");
      do {
        Expression key = expression();
        const bool descending = take("desc");
        if (!descending) {
          take("asc");
        }
        result.order.push_back({std::move(key), descending});
      } while (take(","));
    }
    if (take("limit")) {
      result.limit = read_integer<std::uint64_t>(consume(TokenKind::Number, "row limit"));
    }
    return result;
  }

  Lexer lexer_;
  Token token_;
};

}  // namespace

Statement parse_sql(std::string_view sql) { return Parser(sql).statement(); }

Vector parse_vector(std::string_view text) {
  Parser parser(text);
  Vector result = parser.vector();
  parser.finish();
  return result;
}

}  // namespace minivector
