#include "database.hpp"
#include "request.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <locale>

namespace {

using namespace minivector;

// Keep streamed results bounded while avoiding an ostream operation per token.
class JsonOutput {
public:
  void append(std::string_view value) {
    while (!value.empty()) {
      const auto count = std::min(value.size(), buffer_.size() - used_);
      std::memcpy(buffer_.data() + used_, value.data(), count);
      used_ += count;
      value.remove_prefix(count);
      if (used_ == buffer_.size()) drain();
    }
  }

  void put(char value) {
    buffer_[used_++] = value;
    if (used_ == buffer_.size()) drain();
  }

  template <class Integer>
  void integer(Integer value) {
    char digits[32];
    const auto result = std::to_chars(digits, digits + sizeof(digits), value);
    if (result.ec != std::errc{}) throw std::runtime_error("format JSON integer");
    append({digits, static_cast<std::size_t>(result.ptr - digits)});
  }

  void finish() {
    drain();
    std::cout.flush();
  }

private:
  void drain() {
    std::cout.write(buffer_.data(), static_cast<std::streamsize>(used_));
    used_ = 0;
  }

  std::array<char, 1024> buffer_;
  std::size_t used_ = 0;
};

void json_string(JsonOutput &out, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  out.put('"');
  std::size_t start = 0;
  for (std::size_t i = 0; i < value.size(); ++i) {
    const auto ch = static_cast<unsigned char>(value[i]);
    if (ch >= 0x20 && ch != '"' && ch != '\\') continue;
    out.append(value.substr(start, i - start));
    if (ch == '"' || ch == '\\') {
      out.put('\\');
      out.put(static_cast<char>(ch));
    } else {
      out.append("\\u00");
      out.put(hex[ch >> 4]);
      out.put(hex[ch & 15]);
    }
    start = i + 1;
  }
  out.append(value.substr(start));
  out.put('"');
}

template <class Float>
void json_float(JsonOutput &out, Float value) {
  if (!std::isfinite(value)) throw std::runtime_error("non-finite JSON number");
  char buffer[64];
  const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                       std::chars_format::general,
                                       std::numeric_limits<Float>::max_digits10);
  if (formatted.ec != std::errc{}) throw std::runtime_error("format JSON number");
  out.append({buffer, static_cast<std::size_t>(formatted.ptr - buffer)});
}

void json_value(JsonOutput &out, const Value &value) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    out.integer(*integer);
  } else if (const auto *text = std::get_if<std::string>(&value)) {
    json_string(out, *text);
  } else if (const auto *vector = std::get_if<Vector>(&value)) {
    out.put('[');
    for (std::size_t i = 0; i < vector->size(); ++i) {
      if (i != 0) out.put(',');
      json_float(out, (*vector)[i]);
    }
    out.put(']');
  } else {
    json_float(out, std::get<double>(value));
  }
}

void write_row(JsonOutput &out, const Row &row) {
  out.put('[');
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i != 0) out.put(',');
    json_value(out, row[i]);
  }
  out.put(']');
}

void write_response_prefix(JsonOutput &out, std::string_view id, bool ok) {
  out.append("{\"id\":");
  out.append(id);
  out.append(ok ? ",\"ok\":true" : ",\"ok\":false");
}

void write_query_prefix(JsonOutput &out, std::string_view id, const std::vector<std::string> &columns) {
  write_response_prefix(out, id, true);
  out.append(",\"columns\":[");
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) out.put(',');
    json_string(out, columns[i]);
  }
  out.append("],\"rows\":[");
}

void execute_request(Database &database, JsonOutput &out, const Request &request) {
  if (!request.operation || request_text(*request.operation) != "execute") {
    throw SqlError("UNSUPPORTED_OP", "expected execute or shutdown");
  }
  if (!request.sql) {
    throw SqlError("INVALID_REQUEST", "execute requires a SQL string");
  }
  const auto search_mode = request.search_mode ? request_text(*request.search_mode) : std::string_view{};
  if (search_mode != "exact" && search_mode != "ann") {
    throw SqlError("INVALID_REQUEST", "search_mode must be exact or ann");
  }
  const auto mode = search_mode == "ann" ? SearchMode::Ann : SearchMode::Exact;
  bool first_row = true;
  const auto emit_row = [&](const Row &row) {
    if (!first_row) out.put(',');
    write_row(out, row);
    first_row = false;
  };
  const auto emit_columns = [&](const std::vector<std::string> &columns) {
    write_query_prefix(out, request.id, columns);
  };
  const Execution result = database.execute(request_text(*request.sql), emit_row, mode, emit_columns);
  if (result.columns.empty()) {
    write_response_prefix(out, request.id, true);
    out.append(",\"affected_rows\":");
    out.integer(result.count);
  } else {
    out.put(']');
  }
  out.append("}\n");
  out.finish();
}

int run(const std::filesystem::path &directory, SearchMode default_mode) {
  Database database(directory);
  JsonOutput out;
  std::cout << "{\"type\":\"ready\"}\n" << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::string_view id = "null";
    try {
      const auto request = parse_request(line, default_mode == SearchMode::Ann ? "ann" : "exact");
      id = request.id;
      if (request.operation && request_text(*request.operation) == "shutdown") {
        database.close();
        write_response_prefix(out, id, true);
        out.append("}\n");
        out.finish();
        return 0;
      }
      execute_request(database, out, request);
    } catch (const SqlError &error) {
      write_response_prefix(out, id, false);
      out.append(",\"error\":{\"code\":");
      json_string(out, error.code());
      out.append(",\"message\":");
      json_string(out, error.what());
      out.append("}}\n");
      out.finish();
    }
  }
  database.close();
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);
  std::cout.imbue(std::locale::classic());
  try {
    std::cout.exceptions(std::ios::badbit | std::ios::failbit);
    std::filesystem::path directory = "/data";
    SearchMode mode = SearchMode::Exact;
    for (int i = 1; i < argc; ++i) {
      const std::string_view argument = argv[i];
      if (argument == "--data-dir" && i + 1 < argc) {
        directory = std::filesystem::u8path(argv[++i]);
      } else if (argument == "--search-mode" && i + 1 < argc) {
        const std::string_view search_mode = argv[++i];
        if (search_mode == "exact") {
          mode = SearchMode::Exact;
        } else if (search_mode == "ann") {
          mode = SearchMode::Ann;
        } else {
          throw std::invalid_argument("search mode must be exact or ann");
        }
      } else if (argument == "--help") {
        std::cout << "Usage: minivectordb [--data-dir PATH] [--search-mode exact|ann]\n"
                     "MiniVectorDB JSONL protocol. Send a shutdown request or EOF to close.\n";
        return 0;
      } else {
        throw std::invalid_argument("expected --data-dir, --search-mode or --help");
      }
    }
    return run(directory, mode);
  } catch (const std::exception &error) {
    std::cerr << "minivectordb: " << error.what() << '\n';
    return 1;
  }
}
