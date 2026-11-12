#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace minivector {

struct Request {
  std::string_view id;
  std::optional<std::string> operation;
  std::optional<std::string> sql;
  std::optional<std::string> search_mode;
};

// The returned ID refers to the request line, preserving its JSON representation.
Request parse_request(std::string_view line, std::string_view default_mode);

}  // namespace minivector
