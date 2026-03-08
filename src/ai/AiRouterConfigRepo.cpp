#include "ai/AiRouterConfigRepo.h"

#include <sqlite3.h>

#include <stdexcept>

namespace holder::ai {
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

holder::model::AiRouterConfig read_config(sqlite3_stmt* stmt) {
  holder::model::AiRouterConfig out;
  out.scope = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  if (sqlite3_column_type(stmt, 1) != SQLITE_NULL) {
    out.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  } else {
    out.project_id.reset();
  }
  out.router_model = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  out.updated_at = sqlite3_column_int64(stmt, 3);
  return out;
}

std::string project_key(const std::string& project_id) {
  return std::string("project:") + project_id;
}

} // namespace

AiRouterConfigRepo::AiRouterConfigRepo(holder::store::Db& db) : db_(db) {}

std::optional<holder::model::AiRouterConfig> AiRouterConfigRepo::get_global() const {
  static constexpr const char* SQL =
      "SELECT scope, project_id, router_model, updated_at "
      "FROM ai_router_config WHERE key = 'global';";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get global router config failed");
  }

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto cfg = read_config(stmt);
    sqlite3_finalize(stmt);
    return cfg;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get global router config failed");
  }
  return std::nullopt;
}

std::optional<holder::model::AiRouterConfig> AiRouterConfigRepo::get_for_project(
    const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT scope, project_id, router_model, updated_at "
      "FROM ai_router_config WHERE key = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get project router config failed");
  }
  const std::string key = project_key(project_id);
  bind_text(stmt, 1, key);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto cfg = read_config(stmt);
    sqlite3_finalize(stmt);
    return cfg;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get project router config failed");
  }
  return std::nullopt;
}

void AiRouterConfigRepo::set_global(const std::string& router_model, long long updated_at) {
  static constexpr const char* SQL =
      "INSERT INTO ai_router_config(key, scope, project_id, router_model, updated_at) "
      "VALUES('global', 'global', NULL, ?, ?) "
      "ON CONFLICT(key) DO UPDATE SET "
      "scope = excluded.scope, project_id = excluded.project_id, "
      "router_model = excluded.router_model, updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert global router config failed");
  }

  bind_text(stmt, 1, router_model);
  sqlite3_bind_int64(stmt, 2, updated_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "upsert global router config failed");
  }
  sqlite3_finalize(stmt);
}

void AiRouterConfigRepo::set_for_project(const std::string& project_id,
                                         const std::string& router_model,
                                         long long updated_at) {
  static constexpr const char* SQL =
      "INSERT INTO ai_router_config(key, scope, project_id, router_model, updated_at) "
      "VALUES(?, 'project', ?, ?, ?) "
      "ON CONFLICT(key) DO UPDATE SET "
      "scope = excluded.scope, project_id = excluded.project_id, "
      "router_model = excluded.router_model, updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert project router config failed");
  }

  const std::string key = project_key(project_id);
  bind_text(stmt, 1, key);
  bind_text(stmt, 2, project_id);
  bind_text(stmt, 3, router_model);
  sqlite3_bind_int64(stmt, 4, updated_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "upsert project router config failed");
  }
  sqlite3_finalize(stmt);
}

void AiRouterConfigRepo::clear_global() {
  static constexpr const char* SQL = "DELETE FROM ai_router_config WHERE key = 'global';";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare clear global router config failed");
  }
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "clear global router config failed");
  }
  sqlite3_finalize(stmt);
}

void AiRouterConfigRepo::clear_for_project(const std::string& project_id) {
  static constexpr const char* SQL = "DELETE FROM ai_router_config WHERE key = ?;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare clear project router config failed");
  }
  const std::string key = project_key(project_id);
  bind_text(stmt, 1, key);
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "clear project router config failed");
  }
  sqlite3_finalize(stmt);
}

} // namespace holder::ai
