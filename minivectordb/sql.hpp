#pragma once
#include "storage.hpp"
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <optional>
#include <queue>
#include <set>

namespace mvdb {
inline std::string lower(std::string s) {
  for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}
inline size_t unicode_length(const std::string& s) {
  size_t count = 0;
  for (size_t i = 0; i < s.size();) {
    if (s.size() - i >= sizeof(uint64_t)) {
      uint64_t word; std::memcpy(&word, s.data() + i, sizeof(word));
      if (!(word & 0x8080808080808080ULL)) { i += sizeof(word); count += sizeof(word); continue; }
    }
    ++count;
    auto c = static_cast<unsigned char>(s[i++]);
    if (c < 128) continue;
    unsigned n; uint32_t cp, minimum;
    if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 31; minimum = 128; }
    else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 15; minimum = 2048; }
    else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 7; minimum = 65536; }
    else throw Error("invalid_utf8", "Invalid UTF-8 leading byte");
    require(i + n <= s.size(), "invalid_utf8", "Truncated UTF-8");
    while (n--) {
      auto b = static_cast<unsigned char>(s[i++]);
      require((b & 0xC0) == 0x80, "invalid_utf8", "Invalid UTF-8 continuation");
      cp = (cp << 6) | (b & 63);
    }
    require(cp >= minimum && cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF),
            "invalid_utf8", "Invalid Unicode scalar");
  }
  return count;
}
struct Token { enum Kind {word, number, string, symbol, end} kind; std::string text; };
inline size_t plain_quoted_end(const std::string& sql, size_t at, char quote) {
  constexpr uint64_t ones = 0x0101010101010101ULL, highs = 0x8080808080808080ULL;
  auto has_zero = [&](uint64_t word) { return ((word - ones) & ~word & highs) != 0; };
  const uint64_t quoted = uint64_t(static_cast<unsigned char>(quote)) * ones;
  while (sql.size() - at >= sizeof(uint64_t)) {
    uint64_t word; std::memcpy(&word, sql.data() + at, sizeof(word));
    if (has_zero(word ^ quoted) || (quote == '\'' && has_zero(word ^ (uint64_t('\\') * ones)))) break;
    at += sizeof(word);
  }
  while (at < sql.size() && sql[at] != quote && !(quote == '\'' && sql[at] == '\\')) ++at;
  return at;
}
inline std::vector<Token> tokenize(const std::string& sql) {
  unicode_length(sql);
  std::vector<Token> out;
  for (size_t i = 0; i < sql.size();) {
    unsigned char c = static_cast<unsigned char>(sql[i]);
    if (std::isspace(c)) { ++i; continue; }
    if (sql.compare(i, 2, "--") == 0) { auto e = sql.find('\n', i); i = e == std::string::npos ? sql.size() : e; continue; }
    if (sql.compare(i, 2, "/*") == 0) {
      auto e = sql.find("*/", i + 2); require(e != std::string::npos, "syntax_error", "Unclosed comment"); i = e + 2; continue;
    }
    if (c == '\'' || c == '`' || c == '"') {
      char quote = char(c); ++i; std::string s; bool closed = false;
      while (i < sql.size()) {
        auto end = plain_quoted_end(sql, i, quote);
        s.append(sql, i, end - i); i = end;
        if (i == sql.size()) break;
        char v = sql[i++];
        if (v == quote) {
          if (i < sql.size() && sql[i] == quote) { s += quote; ++i; }
          else { closed = true; break; }
        } else if (v == '\\' && quote == '\'') {
          require(i < sql.size(), "syntax_error", "Unclosed string escape");
          char e = sql[i++];
          switch (e) { case 'n': s += '\n'; break; case 'r': s += '\r'; break;
            case 't': s += '\t'; break; case '0': s += '\0'; break;
            case 'b': s += '\b'; break; case 'Z': s += char(26); break;
            default: s += e; }
        } else s += v;
      }
      require(closed, "syntax_error", "Unclosed quoted token");
      out.push_back({quote == '\'' ? Token::string : Token::word, std::move(s)}); continue;
    }
    if (std::isalpha(c) || c == '_' || c >= 128) {
      size_t start = i++;
      while (i < sql.size()) {
        auto d = static_cast<unsigned char>(sql[i]);
        if (!std::isalnum(d) && d != '_' && d < 128) break;
        ++i;
      }
      out.push_back({Token::word, sql.substr(start, i - start)}); continue;
    }
    if (std::isdigit(c) || ((c == '-' || c == '+') && i + 1 < sql.size() && std::isdigit(static_cast<unsigned char>(sql[i + 1])))) {
      size_t start = i++;
      while (i < sql.size() && (std::isdigit(static_cast<unsigned char>(sql[i])) || sql[i] == '.' ||
             sql[i] == 'e' || sql[i] == 'E' || ((sql[i] == '+' || sql[i] == '-') && (sql[i-1] == 'e' || sql[i-1] == 'E')))) ++i;
      out.push_back({Token::number, sql.substr(start, i - start)}); continue;
    }
    std::string op(1, sql[i++]);
    if (i < sql.size() && (c == '<' || c == '>' || c == '!') && (sql[i] == '=' || (c == '<' && sql[i] == '>'))) op += sql[i++];
    out.push_back({Token::symbol, op});
  }
  out.push_back({Token::end, ""}); return out;
}
inline int64_t integer(const std::string& s) {
  int64_t x = 0; const char* begin = s.data();
  if (!s.empty() && s[0] == '+') ++begin;
  auto r = std::from_chars(begin, s.data() + s.size(), x);
  require(begin != s.data() + s.size() && r.ec == std::errc() && r.ptr == s.data() + s.size(),
          "type_error", "Expected signed BIGINT in range");
  return x;
}
struct Expression { std::string column, name; bool distance = false, star = false; Json argument; };
struct Predicate { std::string column, op; Json value; };
struct Order { Expression expr; bool desc = false; };
struct Statement {
  std::string kind, table, index;
  Json schema = Json::array();
  std::vector<std::string> columns;
  std::vector<std::vector<Json>> values;
  std::vector<std::pair<std::string, Json>> assignments;
  std::vector<Expression> projection;
  std::vector<Predicate> predicates;
  std::vector<Order> order;
  std::optional<uint64_t> limit;
};
class Parser {
  std::vector<Token> t_; size_t at_ = 0;
  bool is(const std::string& text) const { return lower(t_[at_].text) == text && t_[at_].kind != Token::string; }
  bool take(const std::string& text) { if (!is(text)) return false; ++at_; return true; }
  void expect(const std::string& text) { require(take(text), "syntax_error", "Expected " + text); }
  std::string identifier() {
    require(t_[at_].kind == Token::word, "syntax_error", "Expected identifier");
    return lower(t_[at_++].text);
  }
  Json literal() {
    if (t_[at_].kind == Token::string) return t_[at_++].text;
    if (t_[at_].kind == Token::number) return integer(t_[at_++].text);
    throw Error("syntax_error", "Expected literal");
  }
  uint64_t size_value() {
    auto v = literal(); require(v.is_number_integer(), "type_error", "Expected integer");
    auto n = v.get<int64_t>(); require(n >= 0, "type_error", "Negative size"); return uint64_t(n);
  }
  Expression expression() {
    Expression e;
    if (take("*")) { e.star = true; e.name = "*"; return e; }
    if (is("l2_distance") && t_[at_ + 1].text == "(") {
      ++at_; expect("("); e.column = identifier(); expect(","); e.argument = literal(); expect(")");
      e.distance = true; e.name = "L2_DISTANCE(" + e.column + ", " + e.argument.dump() + ")";
    } else { e.column = identifier(); e.name = e.column; }
    return e;
  }
  void where(Statement& s) {
    if (!take("where")) return;
    do {
      Predicate p; p.column = identifier();
      require(t_[at_].kind == Token::symbol, "syntax_error", "Expected comparison operator");
      p.op = t_[at_++].text;
      require(p.op == "=" || p.op == "<" || p.op == ">" || p.op == "<=" || p.op == ">=", "unsupported_sql", "Unsupported comparison");
      p.value = literal(); s.predicates.push_back(std::move(p));
    } while (take("and"));
  }
 public:
  explicit Parser(const std::string& sql) : t_(tokenize(sql)) {}
  Statement parse() {
    Statement s;
    if (take("begin")) { s.kind = "begin"; take("transaction"); }
    else if (take("commit")) s.kind = "commit";
    else if (take("rollback")) s.kind = "rollback";
    else if (take("create")) {
      if (take("vector")) {
        expect("index"); s.kind = "index"; s.index = identifier(); expect("on"); s.table = identifier();
        expect("("); s.columns.push_back(identifier()); expect(")");
      } else {
        expect("table"); s.kind = "create"; s.table = identifier(); expect("(");
        std::string table_pk;
        do {
          if (take("primary")) { expect("key"); expect("("); table_pk = identifier(); expect(")"); continue; }
          auto name = identifier(), type = identifier(); uint64_t size = 0;
          require(type == "bigint" || type == "varchar" || type == "vector", "unsupported_sql", "Unsupported column type");
          if (type != "bigint") { expect("("); size = size_value(); expect(")"); require(size > 0 && size <= 16 * 1024 * 1024, "type_error", "Invalid column dimension"); }
          bool pk = false;
          if (take("primary")) { expect("key"); pk = true; }
          s.schema.push_back({{"name", name}, {"type", type}, {"size", size}, {"primary", pk}});
        } while (take(","));
        expect(")");
        if (!table_pk.empty()) {
          bool found = false;
          for (auto& c : s.schema) if (c["name"] == table_pk) { c["primary"] = true; found = true; }
          require(found, "unknown_column", "Unknown primary key column");
        }
      }
    } else if (take("insert")) {
      s.kind = "insert"; expect("into"); s.table = identifier();
      if (take("(")) { do { s.columns.push_back(identifier()); } while (take(",")); expect(")"); }
      expect("values");
      do { expect("("); std::vector<Json> row; do { row.push_back(literal()); } while (take(",")); expect(")"); s.values.push_back(std::move(row)); } while (take(","));
    } else if (take("select")) {
      s.kind = "select";
      do {
        auto e = expression(); if (take("as")) e.name = identifier();
        s.projection.push_back(std::move(e));
      } while (take(","));
      expect("from"); s.table = identifier(); where(s);
      if (take("order")) { expect("by"); do { Order o; o.expr = expression(); o.desc = take("desc"); if (!o.desc) take("asc"); s.order.push_back(std::move(o)); } while (take(",")); }
      if (take("limit")) s.limit = size_value();
    } else if (take("update")) {
      s.kind = "update"; s.table = identifier(); expect("set");
      do { auto name = identifier(); expect("="); s.assignments.emplace_back(name, literal()); } while (take(",")); where(s);
    } else if (take("delete")) {
      s.kind = "delete"; expect("from"); s.table = identifier(); where(s);
    } else throw Error("unsupported_sql", "Unsupported SQL statement");
    take(";"); require(t_[at_].kind == Token::end, "unsupported_sql", "Unexpected or unsupported SQL suffix");
    return s;
  }
};

inline size_t column_index(const Json& schema, const std::string& name) {
  for (size_t i = 0; i < schema.size(); ++i) if (schema[i]["name"] == name) return i;
  throw Error("unknown_column", "Unknown column: " + name);
}
inline size_t primary_index(const Json& schema) {
  for (size_t i = 0; i < schema.size(); ++i) if (schema[i]["primary"].get<bool>()) return i;
  throw Error("invalid_schema", "Missing primary key");
}
inline Json cast(const Json& input, const Json& column) {
  std::string type = column["type"];
  if (type == "bigint") {
    require(input.is_number_integer() && !input.is_number_unsigned(), "type_error", "Expected signed integer");
    return input;
  }
  if (type == "varchar") {
    require(input.is_string(), "type_error", "Expected VARCHAR");
    require(unicode_length(input.get_ref<const std::string&>()) <= column["size"].get<uint64_t>(), "value_too_long", "VARCHAR length exceeded");
    return input;
  }
  require(input.is_string() || input.is_array(), "invalid_vector", "Expected vector literal");
  Json v;
  try { v = input.is_string() ? Json::parse(input.get_ref<const std::string&>()) : input; }
  catch (const Json::exception&) { throw Error("invalid_vector", "Invalid vector JSON"); }
  require(v.is_array() && v.size() == column["size"].get<uint64_t>(), "invalid_vector", "Vector dimension mismatch");
  for (auto& x : v) {
    require(x.is_number(), "invalid_vector", "Vector component is not numeric");
    double value = x.get<double>(); float stored = static_cast<float>(value);
    require(std::isfinite(value) && std::isfinite(stored), "invalid_vector", "Non-finite float32 vector");
    x = double(stored);
  }
  return v;
}
inline std::vector<unsigned char> encode_row(const Json& row, const Json& schema) {
  std::vector<unsigned char> bytes;
  auto append_word = [&](uint64_t x) { size_t old = bytes.size(); bytes.resize(old + 8); write64(bytes.data() + old, x); };
  for (size_t i = 0; i < schema.size(); ++i) {
    auto type = schema[i]["type"].get<std::string>();
    if (type == "bigint") append_word(uint64_t(row[i].get<int64_t>()));
    else if (type == "varchar") {
      const auto& s = row[i].get_ref<const std::string&>(); append_word(s.size()); bytes.insert(bytes.end(), s.begin(), s.end());
    } else for (const auto& x : row[i]) {
      float v = x.get<float>(); uint32_t word; std::memcpy(&word, &v, 4);
      for (unsigned k = 0; k < 4; ++k) bytes.push_back(static_cast<unsigned char>(word >> (8 * k)));
    }
  }
  return bytes;
}
inline Json decode_row(const std::vector<unsigned char>& bytes, const Json& schema) {
  Json row = Json::array(); size_t at = 0;
  auto word = [&]() { require(at + 8 <= bytes.size(), "corrupt_storage", "Truncated record"); auto x = read64(bytes.data() + at); at += 8; return x; };
  for (const auto& c : schema) {
    auto type = c["type"].get<std::string>();
    if (type == "bigint") row.push_back(int64_t(word()));
    else if (type == "varchar") {
      uint64_t len = word(); require(len <= bytes.size() - at, "corrupt_storage", "Truncated string");
      row.push_back(std::string(reinterpret_cast<const char*>(bytes.data() + at), size_t(len))); at += size_t(len);
    } else {
      size_t n = c["size"].get<size_t>(); require(n <= (bytes.size() - at) / 4, "corrupt_storage", "Truncated vector");
      Json v = Json::array();
      for (size_t i = 0; i < n; ++i) {
        uint32_t b = 0; for (unsigned k = 0; k < 4; ++k) b |= uint32_t(bytes[at++]) << (8 * k);
        float x; std::memcpy(&x, &b, 4); v.push_back(double(x));
      }
      row.push_back(std::move(v));
    }
  }
  require(at == bytes.size(), "corrupt_storage", "Trailing record bytes"); return row;
}
inline int compare(const Json& a, const Json& b) {
  if (a.is_number_integer() && b.is_number_integer()) {
    auto x = a.get<int64_t>(), y = b.get<int64_t>(); return x < y ? -1 : x > y ? 1 : 0;
  }
  if (a.is_number() && b.is_number()) { double x = a.get<double>(), y = b.get<double>(); return x < y ? -1 : x > y ? 1 : 0; }
  require(a.is_string() && b.is_string(), "type_error", "Incompatible comparison");
  const auto& x = a.get_ref<const std::string&>(); const auto& y = b.get_ref<const std::string&>();
  return x < y ? -1 : x > y ? 1 : 0;
}
} // namespace mvdb
