#include "index/Reindexer.h"

#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace holder::index {
namespace {

void throw_sqlite(sqlite3* db, const std::string& what) {
  const char* msg = db ? sqlite3_errmsg(db) : "unknown sqlite error";
  throw std::runtime_error(what + ": " + msg);
}

} // namespace

Reindexer::Reindexer(holder::platform::Db& db) : db_(db) {}

void Reindexer::run() {
  holder::index::FtsIndexer fts(db_);

  db_.exec("DELETE FROM cards_fts;");
  db_.exec("DELETE FROM ai_fts;");

  {
    static constexpr const char* SQL =
        "SELECT card_id, project_id, title, rel_path "
        "FROM cards WHERE deleted_at IS NULL;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
      throw_sqlite(db_.handle(), "prepare reindex cards failed");
    }

    while (true) {
      const int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const auto* card_id = sqlite3_column_text(stmt, 0);
        const auto* project_id = sqlite3_column_text(stmt, 1);
        const auto* title = sqlite3_column_text(stmt, 2);
        const auto* rel_path = sqlite3_column_text(stmt, 3);

        const std::string card_id_str = card_id ? reinterpret_cast<const char*>(card_id) : "";
        const std::string project_id_str = project_id ? reinterpret_cast<const char*>(project_id) : "";
        const std::string title_str = title ? reinterpret_cast<const char*>(title) : "";
        const std::string rel_path_str = rel_path ? reinterpret_cast<const char*>(rel_path) : "";

        fts.upsert_card(card_id_str, project_id_str, title_str, "");
        continue;
      }
      if (rc == SQLITE_DONE) break;
      sqlite3_finalize(stmt); // LCOV_EXCL_LINE
      throw_sqlite(db_.handle(), "reindex cards failed"); // LCOV_EXCL_LINE
    }

    sqlite3_finalize(stmt);
  }

  {
    static constexpr const char* SQL =
        "SELECT m.message_id, m.thread_id, t.project_id, m.content "
        "FROM ai_messages m "
        "JOIN ai_threads t ON t.thread_id = m.thread_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
      throw_sqlite(db_.handle(), "prepare reindex ai messages failed");
    }

    while (true) {
      const int rc = sqlite3_step(stmt);
      if (rc == SQLITE_ROW) {
        const auto* message_id = sqlite3_column_text(stmt, 0);
        const auto* thread_id = sqlite3_column_text(stmt, 1);
        const auto* project_id = sqlite3_column_text(stmt, 2);
        const auto* content = sqlite3_column_text(stmt, 3);

        const std::string message_id_str = message_id ? reinterpret_cast<const char*>(message_id) : "";
        const std::string thread_id_str = thread_id ? reinterpret_cast<const char*>(thread_id) : "";
        const std::string project_id_str = project_id ? reinterpret_cast<const char*>(project_id) : "";
        const std::string content_str = content ? reinterpret_cast<const char*>(content) : "";

        fts.upsert_message(message_id_str, thread_id_str, project_id_str, content_str);
        continue;
      }
      if (rc == SQLITE_DONE) break;
      sqlite3_finalize(stmt); // LCOV_EXCL_LINE
      throw_sqlite(db_.handle(), "reindex ai messages failed"); // LCOV_EXCL_LINE
    }

    sqlite3_finalize(stmt);
  }
}

} // namespace holder::index
