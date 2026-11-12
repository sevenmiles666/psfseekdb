#pragma once
#include "sql.hpp"

namespace mvdb {
struct CompiledExpr { size_t column = 0; std::string name; bool distance = false; Json argument; };
struct CompiledFilter { size_t column; std::string op; Json value; };
struct CompiledOrder { CompiledExpr expression; bool desc; };
struct ResultRow { Entry record; std::vector<Json> sort; size_t sort_bytes = 0; };

class Engine {
  struct VectorEntry { Entry record; std::vector<float> vector; Json scalars; };
  struct VectorCache {
    uint64_t root = 0;
    size_t column = 0, bytes = 0;
    bool complete = false;
    std::vector<VectorEntry> entries;
  };
  File file_;
  Tree tree_;
  Json state_;
  bool transaction_ = false;
  uint64_t transaction_start_ = 0;
  uint64_t commits_since_compact_check_ = 0;
  std::map<std::string, VectorCache> vector_cache_;
  static constexpr size_t vector_budget = 32 * 1024 * 1024;
  void invalidate_vectors() { vector_cache_.clear(); }
  void compact_if_needed() {
    ++commits_since_compact_check_;
    uint64_t threshold = 64 * 1024 * 1024;
    if (const char* configured = std::getenv("MVDB_COMPACT_THRESHOLD")) {
      try { threshold = std::max<uint64_t>(8192, std::stoull(configured)); } catch (...) {}
    }
    if (file_.position() < threshold || (threshold == 64 * 1024 * 1024 && commits_since_compact_check_ < 128)) return;
    commits_since_compact_check_ = 0;
    uint64_t live = 8192;
    for (const auto& item : state_.items()) {
      uint64_t count = 0;
      tree_.scan(item.value()["root"].get<uint64_t>(), [&](const Entry& e) { live += e.length; ++count; return true; });
      live += (count / 60 + 3) * 4096;
    }
    if (file_.position() <= live * 3) return;
    auto temporary = file_.directory() + "/compaction-work";
    std::filesystem::remove_all(temporary);
    {
      File next(temporary); Tree next_tree(next); Json next_state = state_;
      for (auto& item : next_state.items()) {
        uint64_t root = 0;
        tree_.scan(item.value()["root"].get<uint64_t>(), [&](const Entry& old) {
          auto bytes = file_.read(old.position, size_t(old.length));
          auto pos = next.append(bytes.data(), bytes.size());
          root = next_tree.put(root, {old.key, pos, old.length});
          if (next_tree.dirty_count() >= 512) root = next_tree.checkpoint(root);
          return true;
        });
        item.value()["root"] = next_tree.checkpoint(root);
      }
      next.publish(next_state); fault("compaction_written"); file_.replace(next);
      tree_.reset(); invalidate_vectors(); state_ = file_.catalog;
    }
    std::filesystem::remove_all(temporary);
  }
  VectorCache& vector_index(const std::string& name, const Json& t, size_t column) {
    uint64_t root = t["root"].get<uint64_t>();
    auto it = vector_cache_.find(name);
    if (it != vector_cache_.end() && it->second.root == root && it->second.column == column) return it->second;
    // A single shared budget prevents many tables from multiplying cache memory.
    vector_cache_.clear();
    auto& index = vector_cache_[name]; index.root = root; index.column = column; index.complete = true;
    tree_.scan(root, [&](const Entry& e) {
      auto row = read_row(e, t["schema"]);
      VectorEntry v; v.record = e; v.scalars = row; v.scalars[column] = nullptr;
      for (const auto& x : row[column]) v.vector.push_back(x.get<float>());
      size_t bytes = sizeof(VectorEntry) + v.vector.capacity() * sizeof(float) + v.scalars.dump().size() * 4 + 512;
      if (bytes > vector_budget - std::min(index.bytes, vector_budget)) {
        index.entries.clear(); index.entries.shrink_to_fit(); index.complete = false; return false;
      }
      index.bytes += bytes; index.entries.push_back(std::move(v)); return true;
    });
    return index;
  }
  Json& table(const std::string& name) {
    require(state_.contains(name), "unknown_table", "Unknown table: " + name); return state_[name];
  }
  CompiledExpr compile(const Expression& e, const Json& schema) {
    CompiledExpr c; c.column = column_index(schema, e.column); c.name = e.name; c.distance = e.distance;
    if (e.distance) {
      require(schema[c.column]["type"] == "vector", "type_error", "L2_DISTANCE requires VECTOR");
      c.argument = cast(e.argument, schema[c.column]);
    }
    return c;
  }
  Json evaluate(const CompiledExpr& e, const Json& row) const {
    if (!e.distance) return row[e.column];
    double sum = 0;
    for (size_t i = 0; i < e.argument.size(); ++i) {
      double diff = row[e.column][i].get<double>() - e.argument[i].get<double>(); sum += diff * diff;
    }
    return std::sqrt(sum);
  }
  std::vector<CompiledFilter> filters(const Statement& s, const Json& schema) {
    std::vector<CompiledFilter> out;
    for (const auto& p : s.predicates) {
      size_t col = column_index(schema, p.column);
      require(schema[col]["type"] != "vector", "unsupported_sql", "Vector comparison is unsupported");
      if (schema[col]["type"] == "varchar") {
        require(p.value.is_string(), "type_error", "Expected VARCHAR comparison literal");
        unicode_length(p.value.get_ref<const std::string&>());
        out.push_back({col, p.op, p.value});
      } else out.push_back({col, p.op, cast(p.value, schema[col])});
    }
    return out;
  }
  bool matches(const Json& row, const std::vector<CompiledFilter>& filters) const {
    for (const auto& p : filters) {
      int c = compare(row[p.column], p.value);
      bool ok = p.op == "=" ? c == 0 : p.op == "<" ? c < 0 : p.op == ">" ? c > 0 : p.op == "<=" ? c <= 0 : c >= 0;
      if (!ok) return false;
    }
    return true;
  }
  Json read_row(const Entry& entry, const Json& schema) {
    require(entry.length <= 128 * 1024 * 1024, "corrupt_storage", "Record too large");
    return decode_row(file_.read(entry.position, size_t(entry.length)), schema);
  }
  uint64_t store_row(uint64_t root, const Json& row, const Json& schema) {
    auto b = encode_row(row, schema); auto pos = file_.append(b.data(), b.size());
    root = tree_.put(root, {row[primary_index(schema)].get<int64_t>(), pos, b.size()});
    if (tree_.dirty_count() >= 512) root = tree_.checkpoint(root);
    return root;
  }
  void visit(uint64_t root, const Json& schema, const std::vector<CompiledFilter>& conditions,
             const std::function<bool(const Entry&, const Json&)>& callback) {
    size_t pk = primary_index(schema);
    for (const auto& p : conditions) if (p.column == pk && p.op == "=") {
      Entry entry;
      if (tree_.get(root, p.value.get<int64_t>(), entry)) {
        auto row = read_row(entry, schema); if (matches(row, conditions)) callback(entry, row);
      }
      return;
    }
    tree_.scan(root, [&](const Entry& e) { auto row = read_row(e, schema); return !matches(row, conditions) || callback(e, row); });
  }
  Json select(const Statement& s, const Json& t) {
    const auto& schema = t["schema"];
    auto conditions = filters(s, schema);
    std::vector<CompiledExpr> columns;
    for (const auto& e : s.projection) {
      if (e.star) for (const auto& col : schema) {
        Expression c; c.column = col["name"]; c.name = c.column; columns.push_back(compile(c, schema));
      } else columns.push_back(compile(e, schema));
    }
    std::vector<CompiledOrder> order;
    for (auto o : s.order) {
      bool alias = false;
      if (!o.expr.distance) for (const auto& e : s.projection) if (e.name == o.expr.column && !e.star) {
        order.push_back({compile(e, schema), o.desc}); alias = true; break;
      }
      if (!alias) order.push_back({compile(o.expr, schema), o.desc});
    }
    Json names = Json::array(); for (const auto& c : columns) names.push_back(c.name);
    Json answer = {{"columns", names}, {"rows", Json::array()}};
    if (s.limit && *s.limit == 0) return answer;
    uint64_t output_bytes = names.dump().size() + 128;
    const uint64_t max_output = 16 * 1024 * 1024 - 1024;
    auto project = [&](const Json& row) {
      Json values = Json::array();
      for (const auto& col : columns) values.push_back(evaluate(col, row));
      return values;
    };
    auto append_result = [&](Json values) {
      output_bytes += values.dump().size() + 1;
      require(output_bytes <= max_output, "response_too_large", "Response exceeds protocol byte limit");
      answer["rows"].push_back(std::move(values));
    };
    if (order.empty()) {
      visit(t["root"].get<uint64_t>(), schema, conditions, [&](const Entry&, const Json& row) {
        append_result(project(row));
        return !s.limit || answer["rows"].size() < *s.limit;
      });
      return answer;
    }
    for (const auto& o : order) require(o.expression.distance || schema[o.expression.column]["type"] != "vector",
                                      "unsupported_sql", "VECTOR ordering requires L2_DISTANCE");
    bool use_heap = s.limit.has_value() && !order.empty();
    // A bounded, contiguous exact vector index avoids reparsing row vectors for
    // each query. If it does not fit, the same query streams the disk tree.
    if (use_heap && order.size() == 1 && order[0].expression.distance && !order[0].desc && !t["indexes"].empty()) {
      const auto& expr = order[0].expression;
      auto& index = vector_index(s.table, t, expr.column);
      if (index.complete) {
        struct Candidate { double distance; size_t index; int64_t key; };
        auto less = [](const Candidate& a, const Candidate& b) { return a.distance != b.distance ? a.distance < b.distance : a.key < b.key; };
        std::priority_queue<Candidate, std::vector<Candidate>, decltype(less)> top(less);
        std::vector<float> query; for (const auto& x : expr.argument) query.push_back(x.get<float>());
        for (size_t i = 0; i < index.entries.size(); ++i) {
          const auto& e = index.entries[i]; if (!matches(e.scalars, conditions)) continue;
          double sum = 0; for (size_t j = 0; j < query.size(); ++j) { double d = double(e.vector[j]) - double(query[j]); sum += d * d; }
          // Compare the same rounded L2 value exposed by evaluate(). Distinct
          // squared sums can have equal square roots and must then use the PK.
          Candidate c{std::sqrt(sum), i, e.record.key};
          if (top.size() < *s.limit) top.push(c);
          else if (less(c, top.top())) { top.pop(); top.push(c); }
        }
        std::vector<Candidate> candidates;
        while (!top.empty()) { candidates.push_back(top.top()); top.pop(); }
        std::reverse(candidates.begin(), candidates.end());
        for (const auto& c : candidates) {
          append_result(project(read_row(index.entries[c.index].record, schema)));
        }
        return answer;
      }
    }
    const auto pk = primary_index(schema);
    constexpr size_t sort_budget = 8 * 1024 * 1024, key_budget = 4096;
    size_t cached_bytes = 0;
    const bool need_keys = std::any_of(order.begin(), order.end(), [&](const auto& o) {
      return o.expression.distance || o.expression.column != pk;
    });
    auto better = [&](const ResultRow& a, const ResultRow& b) {
      std::optional<Json> loaded_a, loaded_b;
      for (size_t i = 0; i < order.size(); ++i) {
        const auto& expr = order[i].expression;
        int cmp;
        if (!expr.distance && expr.column == pk) {
          cmp = a.record.key < b.record.key ? -1 : a.record.key > b.record.key ? 1 : 0;
        } else {
          Json computed_a, computed_b;
          auto value = [&](const ResultRow& r, std::optional<Json>& loaded, Json& computed) -> const Json& {
            if (!r.sort.empty()) return r.sort[i];
            if (!loaded) loaded = read_row(r.record, schema);
            if (!expr.distance) return (*loaded)[expr.column];
            computed = evaluate(expr, *loaded); return computed;
          };
          cmp = compare(value(a, loaded_a, computed_a), value(b, loaded_b, computed_b));
        }
        if (cmp) return order[i].desc ? cmp > 0 : cmp < 0;
      }
      return a.record.key < b.record.key;
    };
    auto candidate = [&](const Entry& e, const Json& row) {
      ResultRow r{e, {}, 0};
      if (!need_keys || order.size() > key_budget / sizeof(Json)) return r;
      r.sort_bytes = order.size() * sizeof(Json); r.sort.reserve(order.size());
      for (const auto& o : order) {
        const auto& expr = o.expression;
        if (!expr.distance && expr.column == pk) { r.sort.push_back(nullptr); continue; }
        if (!expr.distance && row[expr.column].is_string())
          r.sort_bytes += sizeof(std::string) + row[expr.column].get_ref<const std::string&>().size();
        // Oversized keys stay in immutable records. Comparisons load at most
        // two rows instead of retaining a large text field per candidate.
        if (r.sort_bytes > key_budget || r.sort_bytes > sort_budget - cached_bytes) {
          r.sort = std::vector<Json>{}; r.sort_bytes = 0; return r;
        }
        r.sort.push_back(evaluate(expr, row));
      }
      return r;
    };
    std::vector<ResultRow> rows;
    uint64_t all_result_bytes = output_bytes;
    visit(t["root"].get<uint64_t>(), schema, conditions, [&](const Entry& e, const Json& row) {
      if (!use_heap) {
        // Without LIMIT every match is final, so this check is safe before sort.
        all_result_bytes += project(row).dump().size() + 1;
        require(all_result_bytes <= max_output, "response_too_large", "Response exceeds protocol byte limit");
      }
      auto result = candidate(e, row);
      if (use_heap && rows.size() == *s.limit) {
        if (!better(result, rows.front())) return true;
        std::pop_heap(rows.begin(), rows.end(), better);
        cached_bytes -= rows.back().sort_bytes; rows.pop_back();
      }
      cached_bytes += result.sort_bytes; rows.push_back(std::move(result));
      if (use_heap) std::push_heap(rows.begin(), rows.end(), better);
      // Every projected row needs at least three bytes. Candidate count never
      // decreases, even when individual winners are later replaced.
      require(rows.size() <= max_output / 3, "response_too_large", "Response exceeds protocol byte limit");
      return true;
    });
    std::sort(rows.begin(), rows.end(), better);
    for (const auto& row : rows) append_result(project(read_row(row.record, schema)));
    return answer;
  }
  void validate_schema(const Json& schema) {
    require(!schema.empty(), "invalid_schema", "Empty schema");
    std::set<std::string> names; size_t primary = 0, vectors = 0;
    for (const auto& c : schema) {
      require(names.insert(c["name"].get<std::string>()).second, "invalid_schema", "Duplicate column");
      if (c["primary"].get<bool>()) { ++primary; require(c["type"] == "bigint", "invalid_schema", "Primary key must be BIGINT"); }
      if (c["type"] == "vector") ++vectors;
    }
    require(primary == 1 && vectors <= 1, "invalid_schema", "One primary key and at most one vector are required");
  }
  Json mutate(const Statement& s) {
    if (s.kind == "create") {
      require(!state_.contains(s.table), "table_exists", "Table already exists"); validate_schema(s.schema);
      state_[s.table] = {{"schema", s.schema}, {"root", uint64_t(0)}, {"indexes", Json::object()}};
      return Json::object();
    }
    auto& t = table(s.table); const auto schema = t["schema"]; auto pk = primary_index(schema);
    uint64_t root = t["root"].get<uint64_t>(), affected = 0;
    if (s.kind == "index") {
      size_t i = column_index(schema, s.columns.at(0));
      require(schema[i]["type"] == "vector", "type_error", "Index requires VECTOR column");
      for (const auto& item : state_.items()) require(!item.value()["indexes"].contains(s.index), "index_exists", "Index already exists");
      t["indexes"][s.index] = s.columns[0]; return Json::object();
    }
    if (s.kind == "insert") {
      std::vector<size_t> mapping; std::set<size_t> used;
      if (s.columns.empty()) for (size_t i = 0; i < schema.size(); ++i) mapping.push_back(i);
      else for (const auto& name : s.columns) {
        size_t i = column_index(schema, name); require(used.insert(i).second, "type_error", "Duplicate INSERT column"); mapping.push_back(i);
      }
      require(mapping.size() == schema.size(), "type_error", "INSERT must provide every column");
      for (const auto& values : s.values) {
        require(values.size() == mapping.size(), "type_error", "INSERT value count mismatch");
        Json row = Json::array(); for (size_t i = 0; i < schema.size(); ++i) row.push_back(nullptr);
        for (size_t i = 0; i < values.size(); ++i) row[mapping[i]] = cast(values[i], schema[mapping[i]]);
        Entry existing; require(!tree_.get(root, row[pk].get<int64_t>(), existing), "duplicate_key", "Duplicate primary key");
        root = store_row(root, row, schema); ++affected;
      }
    } else {
      auto conditions = filters(s, schema);
      std::vector<std::pair<size_t, Json>> assignments; std::set<size_t> used;
      for (const auto& [name, value] : s.assignments) {
        size_t i = column_index(schema, name); require(i != pk, "unsupported_sql", "Primary key updates are unsupported");
        require(used.insert(i).second, "type_error", "Duplicate assignment");
        assignments.emplace_back(i, cast(value, schema[i]));
      }
      uint64_t original = root;
      visit(original, schema, conditions, [&](const Entry& e, const Json& before) {
        if (s.kind == "delete") root = tree_.erase(root, e.key);
        else { Json row = before; for (const auto& [i, value] : assignments) row[i] = value; root = store_row(root, row, schema); }
        if (tree_.dirty_count() >= 512) root = tree_.checkpoint(root);
        ++affected; return true;
      });
    }
    root = tree_.checkpoint(root); t["root"] = root;
    return {{"affected_rows", affected}};
  }
 public:
  explicit Engine(const std::string& directory) : file_(directory), tree_(file_), state_(file_.catalog) {
    std::filesystem::remove_all(directory + "/compaction-work");
    for (const auto& item : state_.items()) if (!item.value()["indexes"].empty()) {
      auto name = item.value()["indexes"].begin().value().get<std::string>();
      vector_index(item.key(), item.value(), column_index(item.value()["schema"], name));
    }
  }
  void rollback() {
    require(transaction_, "no_transaction", "No active transaction");
    invalidate_vectors(); file_.truncate(transaction_start_); tree_.reset(); state_ = file_.catalog; transaction_ = false;
  }
  void shutdown() { if (transaction_) rollback(); }
  Json execute(const std::string& sql, const std::string& mode) {
    require(mode == "exact" || mode == "ann", "invalid_mode", "Invalid search mode");
    auto s = Parser(sql).parse();
    if (s.kind == "begin") {
      require(!transaction_, "transaction_active", "Transaction already active");
      transaction_start_ = file_.position(); transaction_ = true; return Json::object();
    }
    if (s.kind == "rollback") { rollback(); return Json::object(); }
    if (s.kind == "commit") {
      require(transaction_, "no_transaction", "No active transaction");
      file_.publish(state_); transaction_ = false; compact_if_needed(); return Json::object();
    }
    if (s.kind == "select") return select(s, table(s.table));
    Json before = state_; auto offset = file_.position();
    Json result;
    try {
      invalidate_vectors();
      result = mutate(s);
      fault("after_statement");
    } catch (const Error& e) {
      // An I/O failure during root publication leaves an uncertain commit.
      // Stop the process rather than acknowledge further writes against it.
      if (e.code == "io_error") throw;
      file_.truncate(offset); tree_.reset(); state_ = std::move(before); throw;
    } catch (...) { file_.truncate(offset); tree_.reset(); state_ = std::move(before); throw; }
    // Publication and compaction are outside the statement rollback scope:
    // once a root is durable, an unrelated maintenance error must not undo it.
    if (!transaction_) { file_.publish(state_); compact_if_needed(); }
    return result;
  }
};
} // namespace mvdb
