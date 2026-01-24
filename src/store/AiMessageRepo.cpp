#include "store/AiMessageRepo.h"

#include <sqlite3.h>

#include <stdexcept>
#include <utility>

namespace holder::store {
namespace {

void throw_sqlite(sqlite3* db, const std::string& what) {
  const char* msg = db ? sqlite3_errmsg(db) : "unknown sqlite error";
  throw std::runtime_error(what + ": " + msg);
}

void bind_text(sqlite3_stmt* stmt, int idx, const std::string& value) {
  if (sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_text failed");
  }
}

void bind_text_optional(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& value) {
  if (value.has_value()) {
    bind_text(stmt, idx, value.value());
  } else if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_null failed");
  }
}

void bind_int64(sqlite3_stmt* stmt, int idx, long long value) {
  if (sqlite3_bind_int64(stmt, idx, static_cast<sqlite3_int64>(value)) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_int64 failed");
  }
}

holder::model::AiMessage read_message(sqlite3_stmt* stmt) {
  holder::model::AiMessage m;
  m.message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  m.thread_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  m.role = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  m.source = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  if (sqlite3_column_type(stmt, 4) == SQLITE_NULL) {
    m.provider.reset();
  } else {
    m.provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  }
  if (sqlite3_column_type(stmt, 5) == SQLITE_NULL) {
    m.model.reset();
  } else {
    m.model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  }
  m.content = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  m.created_at = sqlite3_column_int64(stmt, 7);
  if (sqlite3_column_type(stmt, 8) == SQLITE_NULL) {
    m.prompt_hash.reset();
  } else {
    m.prompt_hash = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 8));
  }
  if (sqlite3_column_type(stmt, 9) == SQLITE_NULL) {
    m.meta_json.reset();
  } else {
    m.meta_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
  }
  return m;
}

} // namespace

AiMessageRepo::AiMessageRepo(Db& db, holder::index::FtsIndexer* fts) : db_(db), fts_(fts) {}

void AiMessageRepo::append(const holder::model::AiMessage& message) {
  static constexpr const char* SQL =
      "INSERT INTO ai_messages(message_id, thread_id, role, source, provider, model, content, "
      "created_at, prompt_hash, meta_json) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert ai message failed");
  }

  bind_text(stmt, 1, message.message_id);
  bind_text(stmt, 2, message.thread_id);
  bind_text(stmt, 3, message.role);
  bind_text(stmt, 4, message.source);
  bind_text_optional(stmt, 5, message.provider);
  bind_text_optional(stmt, 6, message.model);
  bind_text(stmt, 7, message.content);
  bind_int64(stmt, 8, message.created_at);
  bind_text_optional(stmt, 9, message.prompt_hash);
  bind_text_optional(stmt, 10, message.meta_json);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "insert ai message failed");
  }

  if (fts_) {
    static constexpr const char* SQL_PROJECT =
        "SELECT project_id FROM ai_threads WHERE thread_id = ? LIMIT 1;";
    sqlite3_stmt* s = nullptr;
    if (sqlite3_prepare_v2(db_.handle(), SQL_PROJECT, -1, &s, nullptr) != SQLITE_OK) {
      throw_sqlite(db_.handle(), "prepare fetch thread project_id failed");
    }
    bind_text(s, 1, message.thread_id);
    const int rc2 = sqlite3_step(s);
    if (rc2 == SQLITE_ROW) {
      const auto* text = sqlite3_column_text(s, 0);
      const std::string project_id = text ? reinterpret_cast<const char*>(text) : "";
      sqlite3_finalize(s);
      fts_->upsert_message(message.message_id, message.thread_id, project_id, message.content);
    } else {
      sqlite3_finalize(s);
      throw std::runtime_error("thread not found for ai message");
    }
  }
}

std::vector<holder::model::AiMessage> AiMessageRepo::list_by_thread(
    const std::string& thread_id) const {
  static constexpr const char* SQL =
      "SELECT message_id, thread_id, role, source, provider, model, content, created_at, prompt_hash, meta_json "
      "FROM ai_messages WHERE thread_id = ? ORDER BY created_at ASC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list ai messages failed");
  }

  bind_text(stmt, 1, thread_id);

  std::vector<holder::model::AiMessage> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_message(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "list ai messages failed");
  }

  sqlite3_finalize(stmt);
  return out;
}

} // namespace holder::store
