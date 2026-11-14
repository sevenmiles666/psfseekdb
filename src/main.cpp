#include "database.hpp"
#include "request.hpp"

#include <charconv>
#include <cmath>
#include <iostream>
#include <limits>
#include <locale>

namespace {

using namespace minivector;

void json_string(std::ostream &out, std::string_view value) {
  constexpr char hex[] = "0123456789abcdef";
  out << '"';
  for (const unsigned char ch : value) {
    if (ch == '"' || ch == '\\') {
      out << '\\' << static_cast<char>(ch);
    } else if (ch < 0x20) {
      out << "\\u00" << hex[ch >> 4] << hex[ch & 15];
    } else {
      out << static_cast<char>(ch);
    }
  }
  out << '"';
}

template <class Float>
void json_float(std::ostream &out, Float value) {
  if (!std::isfinite(value)) throw std::runtime_error("non-finite JSON number");
  char buffer[64];
  const auto formatted = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                       std::chars_format::general,
                                       std::numeric_limits<Float>::max_digits10);
  if (formatted.ec != std::errc{}) throw std::runtime_error("format JSON number");
  out.write(buffer, formatted.ptr - buffer);
}

void json_value(std::ostream &out, const Value &value) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    out << *integer;
  } else if (const auto *text = std::get_if<std::string>(&value)) {
    json_string(out, *text);
  } else if (const auto *vector = std::get_if<Vector>(&value)) {
    out << '[';
    for (std::size_t i = 0; i < vector->size(); ++i) {
      if (i != 0) out << ',';
      json_float(out, (*vector)[i]);
    }
    out << ']';
  } else {
    json_float(out, std::get<double>(value));
  }
}

void write_row(const Row &row) {
  std::cout << '[';
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i != 0) std::cout << ',';
    json_value(std::cout, row[i]);
  }
  std::cout << ']';
}

void write_response_prefix(std::string_view id, bool ok) {
  std::cout << "{\"id\":" << id << ",\"ok\":" << (ok ? "true" : "false");
}

void write_query_prefix(std::string_view id, const std::vector<std::string> &columns) {
  write_response_prefix(id, true);
  std::cout << ",\"columns\":[";
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) std::cout << ',';
    json_string(std::cout, columns[i]);
  }
  std::cout << "],\"rows\":[";
}

void execute_request(Database &database, const Request &request) {
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
    if (!first_row) std::cout << ',';
    write_row(row);
    first_row = false;
  };
  const auto emit_columns = [&](const std::vector<std::string> &columns) {
    write_query_prefix(request.id, columns);
  };
  const Execution result = database.execute(request_text(*request.sql), emit_row, mode, emit_columns);
  if (result.columns.empty()) {
    write_response_prefix(request.id, true);
    std::cout << ",\"affected_rows\":" << result.count;
  } else {
    std::cout << ']';
  }
  std::cout << "}\n" << std::flush;
}

int run(const std::filesystem::path &directory, SearchMode default_mode) {
  Database database(directory);
  std::cout << "{\"type\":\"ready\"}\n" << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    std::string_view id = "null";
    try {
      const auto request = parse_request(line, default_mode == SearchMode::Ann ? "ann" : "exact");
      id = request.id;
      if (request.operation && request_text(*request.operation) == "shutdown") {
        database.close();
        write_response_prefix(id, true);
        std::cout << "}\n" << std::flush;
        return 0;
      }
      execute_request(database, request);
    } catch (const SqlError &error) {
      write_response_prefix(id, false);
      std::cout << ",\"error\":{\"code\":";
      json_string(std::cout, error.code());
      std::cout << ",\"message\":";
      json_string(std::cout, error.what());
      std::cout << "}}\n" << std::flush;
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
