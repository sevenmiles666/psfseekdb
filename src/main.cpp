#include "database.hpp"

#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>

namespace {

using namespace minivector;

void json_string(std::ostream &out, const std::string &value) {
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

void json_value(std::ostream &out, const Value &value) {
  if (const auto *integer = std::get_if<std::int64_t>(&value)) {
    out << *integer;
  } else if (const auto *text = std::get_if<std::string>(&value)) {
    json_string(out, *text);
  } else if (const auto *vector = std::get_if<Vector>(&value)) {
    out << '[' << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (std::size_t i = 0; i < vector->size(); ++i) {
      if (i != 0) out << ',';
      out << (*vector)[i];
    }
    out << ']';
  } else {
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::get<double>(value);
  }
}

void write_row(const Row &row) {
  std::cout << "ROW\t[";
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (i != 0) std::cout << ',';
    json_value(std::cout, row[i]);
  }
  std::cout << "]\n";
}

int run(const std::filesystem::path &directory) {
  Database database(directory);
  // This framing is only for local tests. The official driver can call execute().
  std::cout << "READY\tminivectordb-local-v1\n" << std::flush;
  std::string line;
  while (std::getline(std::cin, line)) {
    const auto first = line.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      continue;
    }
    const auto last = line.find_last_not_of(" \t\r\n");
    const auto statement = std::string_view(line).substr(first, last - first + 1);
    if (statement == "\\q") {
      break;
    }
    try {
      const Execution result = database.execute(statement, write_row);
      std::cout << "OK\t" << result.count << '\n' << std::flush;
    } catch (const SqlError &error) {
      std::cout << "ERR\t" << error.code() << '\t';
      json_string(std::cout, error.what());
      std::cout << '\n' << std::flush;
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
    for (int i = 1; i < argc; ++i) {
      const std::string_view argument = argv[i];
      if (argument == "--data-dir" && i + 1 < argc) {
        directory = std::filesystem::u8path(argv[++i]);
      } else if (argument == "--help") {
        std::cout << "Usage: minivectordb [--data-dir PATH]\n"
                     "Local test protocol: one SQL statement per input line. EOF or \\q closes the database.\n";
        return 0;
      } else {
        throw std::invalid_argument("expected --data-dir PATH or --help");
      }
    }
    return run(directory);
  } catch (const std::exception &error) {
    std::cerr << "minivectordb: " << error.what() << '\n';
    return 1;
  }
}
