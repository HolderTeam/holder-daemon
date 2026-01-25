#include "store/AiMessageRepo.h"

#include "core/AiMessageFrontMatter.h"
#include "core/AiMessagePaths.h"

#include <sqlite3.h>

#include <filesystem>
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

AiMessageRepo::AiMessageRepo(Db& db, holder::index::FtsIndexer* fts)
    : db_(db), repo_(), thread_repo_(db), project_repo_(db), fts_(fts) {}

void AiMessageRepo::append(const holder::model::AiMessage& message) {
  const auto thread_opt = thread_repo_.get(message.thread_id);
  if (!thread_opt.has_value()) {
    throw std::runtime_error("thread not found for ai message");
  }
  const auto project_opt = project_repo_.get(thread_opt->project_id);
  if (!project_opt.has_value()) {
    throw std::runtime_error("project not found for ai message thread");
  }

  repo_.open_or_init(project_opt->root_path);
  if (project_opt->git_remote_url.has_value()) {
    repo_.set_remote("origin", project_opt->git_remote_url.value());
  }

  const std::string rel_path = holder::core::ai_message_rel_path(message.message_id);
  const auto full_path = repo_.repo_dir() / rel_path;
  if (std::filesystem::exists(full_path)) {
    throw std::runtime_error("conflict: ai message file already exists");
  }

  const auto front_matter = holder::core::render_ai_message_front_matter(message, project_opt->project_id);
  repo_.write_file(rel_path, front_matter + message.content);

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
    std::filesystem::remove(full_path);
    throw_sqlite(db_.handle(), "insert ai message failed");
  }

  if (fts_) {
    fts_->upsert_message(message.message_id,
                         message.thread_id,
                         project_opt->project_id,
                         message.content);
  }

  repo_.stage_path(rel_path);
  repo_.commit("Add ai message " + message.message_id);
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

std::optional<holder::model::AiMessage> AiMessageRepo::get(
    const std::string& message_id) const {
  static constexpr const char* SQL =
      "SELECT message_id, thread_id, role, source, provider, model, content, created_at, prompt_hash, meta_json "
      "FROM ai_messages WHERE message_id = ? LIMIT 1;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get ai message failed");
  }

  bind_text(stmt, 1, message_id);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto message = read_message(stmt);
    sqlite3_finalize(stmt);
    return message;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) {
    return std::nullopt;
  }

  throw_sqlite(db_.handle(), "get ai message failed");
  return std::nullopt;
}

} // namespace holder::store
