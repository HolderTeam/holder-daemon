#include "api/support/ThreadCompaction.h"

#include "api/support/CloudClient.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <set>
#include <stdexcept>

namespace holder::api::support {
namespace {

std::string column_text(sqlite3_stmt* stmt, int index) {
  const auto* text = sqlite3_column_text(stmt, index);
  if (!text) return {};
  return reinterpret_cast<const char*>(text);
}

std::optional<std::string> column_nullable(sqlite3_stmt* stmt, int index) {
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) return std::nullopt;
  return column_text(stmt, index);
}

void throw_sqlite(sqlite3* db, const std::string& msg) {
  throw std::runtime_error(msg + ": " + sqlite3_errmsg(db));
}

std::string truncate_bytes_tail(const std::string& text, size_t max_bytes) {
  if (text.size() <= max_bytes) return text;
  return text.substr(text.size() - max_bytes);
}

std::vector<std::string> parse_pinned_facts(const std::optional<std::string>& raw_json) {
  std::vector<std::string> out;
  if (!raw_json.has_value() || raw_json->empty()) return out;
  try {
    const auto parsed = nlohmann::json::parse(raw_json.value());
    if (!parsed.is_array()) return out;
    for (const auto& item : parsed) {
      if (item.is_string()) {
        const auto text = item.get<std::string>();
        if (!text.empty()) out.push_back(text);
      }
    }
  } catch (const std::exception&) {
    return {};
  }
  return out;
}

std::optional<std::string> extract_pinned_facts_json_from_context(const std::string& context_json) {
  if (context_json.empty()) return std::nullopt;
  try {
    const auto parsed = nlohmann::json::parse(context_json);
    if (parsed.is_object() && parsed.contains("pinned_facts") && parsed.at("pinned_facts").is_array()) {
      return parsed.at("pinned_facts").dump();
    }
  } catch (const std::exception&) {
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string> merge_pinned_facts_json(const std::optional<std::string>& current_json,
                                                   const std::optional<std::string>& incoming_json) {
  std::set<std::string> dedupe;
  std::vector<std::string> merged;

  const auto append = [&](const std::vector<std::string>& values) {
    for (const auto& value : values) {
      if (value.empty()) continue;
      if (dedupe.insert(value).second) {
        merged.push_back(value);
      }
      if (merged.size() >= 24) return;
    }
  };

  append(parse_pinned_facts(current_json));
  append(parse_pinned_facts(incoming_json));

  if (merged.empty()) return std::nullopt;
  return nlohmann::json(merged).dump();
}

} // namespace

std::optional<ThreadCompactionState> load_thread_compaction_state(holder::store::Db& db,
                                                                  const std::string& thread_id) {
  static constexpr const char* SQL =
      "SELECT thread_id, rolling_summary, pinned_facts_json, last_compacted_message_id, updated_at "
      "FROM ai_thread_compaction_state "
      "WHERE thread_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db.handle(), "prepare thread compaction get failed");
  }
  sqlite3_bind_text(stmt, 1, thread_id.c_str(), -1, SQLITE_TRANSIENT);

  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  ThreadCompactionState out;
  out.thread_id = column_text(stmt, 0);
  out.rolling_summary = column_nullable(stmt, 1);
  out.pinned_facts_json = column_nullable(stmt, 2);
  out.last_compacted_message_id = column_nullable(stmt, 3);
  out.updated_at = sqlite3_column_int64(stmt, 4);
  sqlite3_finalize(stmt);
  return out;
}

void upsert_thread_compaction_state(holder::store::Db& db, const ThreadCompactionState& state) {
  static constexpr const char* SQL =
      "INSERT INTO ai_thread_compaction_state("
      "thread_id, rolling_summary, pinned_facts_json, last_compacted_message_id, updated_at) "
      "VALUES(?, ?, ?, ?, ?) "
      "ON CONFLICT(thread_id) DO UPDATE SET "
      "rolling_summary = excluded.rolling_summary, "
      "pinned_facts_json = excluded.pinned_facts_json, "
      "last_compacted_message_id = excluded.last_compacted_message_id, "
      "updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db.handle(), "prepare thread compaction upsert failed");
  }
  sqlite3_bind_text(stmt, 1, state.thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (state.rolling_summary.has_value()) {
    sqlite3_bind_text(stmt, 2, state.rolling_summary->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 2);
  }
  if (state.pinned_facts_json.has_value()) {
    sqlite3_bind_text(stmt, 3, state.pinned_facts_json->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 3);
  }
  if (state.last_compacted_message_id.has_value()) {
    sqlite3_bind_text(stmt, 4, state.last_compacted_message_id->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, 4);
  }
  sqlite3_bind_int64(stmt, 5, state.updated_at);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db.handle(), "upsert thread compaction failed");
  }
}

std::string build_compacted_context(const std::string& context_json,
                                    long long allowed_context_tokens,
                                    const std::optional<ThreadCompactionState>& state,
                                    bool* compacted,
                                    bool* used_summary,
                                    int* pinned_fact_count) {
  if (compacted) *compacted = false;
  if (used_summary) *used_summary = false;
  if (pinned_fact_count) *pinned_fact_count = 0;
  if (allowed_context_tokens <= 0) {
    if (compacted) *compacted = !context_json.empty();
    return {};
  }

  const long long max_bytes_ll = allowed_context_tokens * 4;
  if (max_bytes_ll <= 0) return {};
  const size_t max_bytes = static_cast<size_t>(max_bytes_ll);

  std::string prefix;
  if (state.has_value()) {
    if (state->rolling_summary.has_value() && !state->rolling_summary->empty()) {
      std::string summary = "[rolling_summary]\n" + state->rolling_summary.value() + "\n\n";
      summary = truncate_bytes_tail(summary, std::min(max_bytes, static_cast<size_t>(2400)));
      prefix += summary;
      if (used_summary) *used_summary = true;
    }

    auto facts = parse_pinned_facts(state->pinned_facts_json);
    if (!facts.empty()) {
      std::string facts_block = "[pinned_facts]\n";
      int count = 0;
      for (const auto& fact : facts) {
        if (count >= 12) break;
        facts_block += "- " + fact + "\n";
        ++count;
      }
      if (count > 0) {
        facts_block += "\n";
        facts_block = truncate_bytes_tail(facts_block, std::min(max_bytes, static_cast<size_t>(1200)));
        prefix += facts_block;
        if (pinned_fact_count) *pinned_fact_count = count;
      }
    }
  }

  size_t remaining = (prefix.size() >= max_bytes) ? 0 : (max_bytes - prefix.size());
  bool tail_compacted = false;
  std::string tail = compact_context_tail(
      context_json, static_cast<long long>(remaining / 4), &tail_compacted);
  if (tail_compacted) {
    if (compacted) *compacted = true;
  }
  if (!prefix.empty()) {
    if (compacted) *compacted = true;
  }

  std::string out = prefix + tail;
  if (out.size() > max_bytes) {
    if (prefix.size() >= max_bytes) {
      out = prefix.substr(0, max_bytes);
    } else {
      const size_t keep_tail = max_bytes - prefix.size();
      if (tail.size() > keep_tail) {
        tail = tail.substr(tail.size() - keep_tail);
      }
      out = prefix + tail;
    }
    if (compacted) *compacted = true;
  }
  return out;
}

void roll_thread_compaction_state(holder::store::Db& db,
                                  const std::string& thread_id,
                                  const std::string& context_json,
                                  long long updated_at) {
  if (thread_id.empty()) return;

  ThreadCompactionState next;
  next.thread_id = thread_id;
  next.updated_at = updated_at;

  const auto current = load_thread_compaction_state(db, thread_id);
  if (current.has_value()) {
    next = current.value();
    next.updated_at = updated_at;
  }

  const std::string context_tail =
      compact_context_tail(context_json, /*allowed_context_tokens=*/800, nullptr);
  std::string rolling = next.rolling_summary.value_or("");
  if (!context_tail.empty()) {
    if (!rolling.empty()) rolling += "\n";
    rolling += context_tail;
    if (rolling.size() > 6000) {
      rolling = truncate_bytes_tail(rolling, 6000);
    }
  }
  if (!rolling.empty()) {
    next.rolling_summary = rolling;
  }

  const auto incoming_facts = extract_pinned_facts_json_from_context(context_json);
  next.pinned_facts_json = merge_pinned_facts_json(next.pinned_facts_json, incoming_facts);

  upsert_thread_compaction_state(db, next);
}

} // namespace holder::api::support
