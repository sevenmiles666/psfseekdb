#include "engine.hpp"
#include <iostream>

using mvdb::Json;
static void emit(const Json& value) { std::cout << value.dump() << '\n' << std::flush; }
static Json strict_json(const std::string& line) {
  std::vector<std::set<std::string>> keys;
  auto callback = [&](int, Json::parse_event_t event, Json& value) {
    if (event == Json::parse_event_t::object_start) keys.emplace_back();
    if (event == Json::parse_event_t::key) mvdb::require(keys.back().insert(value.get<std::string>()).second,
                                                      "invalid_request", "Duplicate JSON key");
    if (event == Json::parse_event_t::object_end) keys.pop_back();
    return true;
  };
  return Json::parse(line, callback);
}
int main(int argc, char** argv) {
  try {
    std::string data = "/data";
    if (argc == 3 && std::string(argv[1]) == "--data-dir") data = argv[2];
    else mvdb::require(argc == 1, "invalid_argument", "Usage: minivectordb [--data-dir directory]");
    mvdb::Engine engine(data); emit({{"type", "ready"}});
    for (std::string line; std::getline(std::cin, line);) {
      Json id = nullptr;
      try {
        auto request = strict_json(line);
        mvdb::require(request.is_object() && request.contains("id"), "invalid_request", "Missing request id");
        id = request["id"];
        auto op = request.at("op").get<std::string>();
        if (op == "shutdown") { engine.shutdown(); emit({{"id", id}, {"ok", true}}); return 0; }
        mvdb::require(op == "execute", "unsupported_op", "Unsupported request operation");
        auto result = engine.execute(request.at("sql").get<std::string>(), request.value("search_mode", std::string("exact")));
        result["id"] = id; result["ok"] = true; emit(result);
      } catch (const mvdb::Error& e) {
        emit({{"id", id}, {"ok", false}, {"error", {{"code", e.code}, {"message", e.what()}}}});
        if (e.code == "io_error" || e.code == "corrupt_storage") return 1;
      } catch (const Json::exception& e) {
        emit({{"id", id}, {"ok", false}, {"error", {{"code", "invalid_request"}, {"message", e.what()}}}});
      }
    }
    engine.shutdown(); return 0;
  } catch (const std::exception& e) { std::cerr << "fatal: " << e.what() << '\n'; return 1; }
}
