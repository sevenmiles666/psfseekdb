#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace minivector {

using RequestText = std::variant<std::string_view, std::string>;

inline std::string_view request_text(const RequestText &text) {
  if (const auto *view = std::get_if<std::string_view>(&text)) return *view;
  return std::get<std::string>(text);
}

struct Request {
  std::string_view id;
  std::optional<RequestText> operation;
  std::optional<RequestText> sql;
  std::optional<RequestText> search_mode;
};

// Keep the line alive while using the ID and any unescaped string fields.
Request parse_request(std::string_view line, std::string_view default_mode);

}  // namespace minivector
