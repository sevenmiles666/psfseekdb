#include "request.hpp"

#include "sql.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace minivector {
namespace {

[[noreturn]] void invalid(const char *message) {
  throw SqlError("INVALID_REQUEST", message);
}

class JsonReader {
public:
  explicit JsonReader(std::string_view text) : text_(text) {}

  Request request(std::string_view default_mode) {
    Request result;
    result.search_mode = default_mode;
    object([&](std::string_view key) {
      if (key == "id") {
        skip_space();
        const std::size_t start = position_;
        value(0);
        result.id = text_.substr(start, position_ - start);
      } else if (key == "op") {
        result.operation = text_field();
      } else if (key == "sql") {
        result.sql = text_field();
      } else if (key == "search_mode") {
        result.search_mode = text_field();
      } else {
        value(0);
      }
    });
    skip_space();
    if (position_ != text_.size()) invalid("unexpected trailing JSON data");
    if (result.id.empty()) invalid("request requires an id");
    return result;
  }

private:
  char current() const {
    return position_ < text_.size() ? text_[position_] : '\0';
  }

  char read() {
    if (position_ == text_.size()) invalid("incomplete JSON value");
    return text_[position_++];
  }

  void skip_space() {
    while (current() == ' ' || current() == '\t' || current() == '\r' || current() == '\n') {
      ++position_;
    }
  }

  bool take(char token) {
    skip_space();
    if (current() != token) return false;
    ++position_;
    return true;
  }

  void expect(char token) {
    if (!take(token)) invalid("unexpected JSON token");
  }

  template <class Field>
  void object(Field field) {
    expect('{');
    if (take('}')) return;
    std::vector<RequestText> keys;
    do {
      auto key = string();
      const auto name = request_text(key);
      if (std::any_of(keys.begin(), keys.end(), [&](const RequestText &previous) {
            return request_text(previous) == name;
          })) invalid("duplicate JSON key");
      expect(':');
      field(name);
      keys.push_back(std::move(key));
    } while (take(','));
    expect('}');
  }

  std::optional<RequestText> text_field() {
    skip_space();
    if (current() == '"') return string();
    value(0);
    return std::nullopt;
  }

  RequestText string() {
    expect('"');
    std::string result;
    std::size_t start = position_;
    while (position_ < text_.size()) {
      if (text_.size() - position_ >= sizeof(std::uint64_t)) {
        std::uint64_t word;
        std::memcpy(&word, text_.data() + position_, sizeof(word));
        constexpr auto has_zero_byte = [](std::uint64_t bytes) {
          return ((bytes - 0x0101010101010101ULL) & ~bytes & 0x8080808080808080ULL) != 0;
        };
        // The three high bits are all zero for JSON control bytes.
        if (!has_zero_byte(word & 0xe0e0e0e0e0e0e0e0ULL) &&
            !has_zero_byte(word ^ 0x2222222222222222ULL) &&
            !has_zero_byte(word ^ 0x5c5c5c5c5c5c5c5cULL)) {
          position_ += sizeof(word);
          continue;
        }
      }
      const auto ch = static_cast<unsigned char>(read());
      if (ch < 0x20) invalid("unescaped JSON control character");
      if (ch == '"' || ch == '\\') {
        const auto segment = text_.substr(start, position_ - start - 1);
        if (ch == '"' && result.empty()) return segment;
        result.append(segment);
        if (ch == '"') return result;
        append_escape(result);
        start = position_;
      }
    }
    invalid("unterminated JSON string");
  }

  void append_escape(std::string &result) {
    switch (const char escape = read()) {
      case '"':
      case '\\':
      case '/': result.push_back(escape); return;
      case 'b': result.push_back('\b'); return;
      case 'f': result.push_back('\f'); return;
      case 'n': result.push_back('\n'); return;
      case 'r': result.push_back('\r'); return;
      case 't': result.push_back('\t'); return;
      case 'u': append_codepoint(result); return;
      default: invalid("invalid JSON string escape");
    }
  }

  std::uint32_t hex_quad() {
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      const char ch = read();
      result <<= 4;
      if (ch >= '0' && ch <= '9') result |= ch - '0';
      else if (ch >= 'a' && ch <= 'f') result |= ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F') result |= ch - 'A' + 10;
      else invalid("invalid Unicode escape");
    }
    return result;
  }

  void append_codepoint(std::string &result) {
    std::uint32_t code = hex_quad();
    if (code >= 0xd800 && code <= 0xdbff) {
      if (read() != '\\' || read() != 'u') invalid("missing Unicode surrogate");
      const auto low = hex_quad();
      if (low < 0xdc00 || low > 0xdfff) invalid("invalid Unicode surrogate");
      code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      invalid("unexpected Unicode surrogate");
    }
    const auto continuation = [&](unsigned shift) {
      result.push_back(static_cast<char>(0x80 | ((code >> shift) & 0x3f)));
    };
    if (code < 0x80) {
      result.push_back(static_cast<char>(code));
    } else if (code < 0x800) {
      result.push_back(static_cast<char>(0xc0 | (code >> 6)));
      continuation(0);
    } else if (code < 0x10000) {
      result.push_back(static_cast<char>(0xe0 | (code >> 12)));
      continuation(6);
      continuation(0);
    } else {
      result.push_back(static_cast<char>(0xf0 | (code >> 18)));
      continuation(12);
      continuation(6);
      continuation(0);
    }
  }

  void literal(std::string_view token) {
    if (text_.substr(position_, token.size()) != token) invalid("invalid JSON value");
    position_ += token.size();
  }

  void digits() {
    const auto start = position_;
    while (current() >= '0' && current() <= '9') ++position_;
    if (position_ == start) invalid("invalid JSON number");
  }

  void number() {
    if (current() == '-') ++position_;
    if (current() == '0') ++position_;
    else digits();
    if (current() == '.') {
      ++position_;
      digits();
    }
    if (current() == 'e' || current() == 'E') {
      ++position_;
      if (current() == '-' || current() == '+') ++position_;
      digits();
    }
  }

  void value(unsigned depth) {
    if (depth >= 64) invalid("JSON value is nested too deeply");
    skip_space();
    switch (current()) {
      case '"': string(); return;
      case 't': literal("true"); return;
      case 'f': literal("false"); return;
      case 'n': literal("null"); return;
      case '{':
        object([&](std::string_view) { value(depth + 1); });
        return;
      case '[':
        ++position_;
        if (take(']')) return;
        do {
          value(depth + 1);
        } while (take(','));
        expect(']');
        return;
      default: number(); return;
    }
  }

  std::string_view text_;
  std::size_t position_ = 0;
};

}  // namespace

Request parse_request(std::string_view line, std::string_view default_mode) {
  return JsonReader(line).request(default_mode);
}

}  // namespace minivector
