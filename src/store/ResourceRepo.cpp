#include "store/ResourceRepo.h"

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

holder::model::Resource read_resource(sqlite3_stmt* stmt) {
  holder::model::Resource r;
  r.resource_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  r.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  r.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  r.uri = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
  r.label = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  if (sqlite3_column_type(stmt, 5) == SQLITE_NULL) {
    r.desc.reset();
  } else {
    r.desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  }
  r.created_at = sqlite3_column_int64(stmt, 6);
  r.updated_at = sqlite3_column_int64(stmt, 7);
  return r;
}

} // namespace

ResourceRepo::ResourceRepo(Db& db) : db_(db) {}

void ResourceRepo::add(const holder::model::Resource& resource) {
  static constexpr const char* SQL =
      "INSERT INTO resources(resource_id, project_id, kind, uri, label, desc, created_at, updated_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare insert resource failed");
  }

  bind_text(stmt, 1, resource.resource_id);
  bind_text(stmt, 2, resource.project_id);
  bind_text(stmt, 3, resource.kind);
  bind_text(stmt, 4, resource.uri);
  bind_text(stmt, 5, resource.label);
  bind_text_optional(stmt, 6, resource.desc);
  bind_int64(stmt, 7, resource.created_at);
  bind_int64(stmt, 8, resource.updated_at);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "insert resource failed");
  }
}

std::vector<holder::model::Resource> ResourceRepo::list(const std::string& project_id) const {
  static constexpr const char* SQL =
      "SELECT resource_id, project_id, kind, uri, label, desc, created_at, updated_at "
      "FROM resources WHERE project_id = ? ORDER BY updated_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare list resources failed");
  }

  bind_text(stmt, 1, project_id);

  std::vector<holder::model::Resource> out;
  while (true) {
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
      out.push_back(read_resource(stmt));
      continue;
    }
    if (rc == SQLITE_DONE) break;
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "list resources failed");
  }

  sqlite3_finalize(stmt);
  return out;
}

void ResourceRepo::remove(const std::string& resource_id) {
  static constexpr const char* SQL = "DELETE FROM resources WHERE resource_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare delete resource failed");
  }

  bind_text(stmt, 1, resource_id);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "delete resource failed");
  }
}

} // namespace holder::store
