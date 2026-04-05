#include "platform/Db.h"

#include <stdexcept>

namespace holder::platform {

Db::~Db() { close(); }

Db::Db(Db&& other) noexcept {
  db_ = other.db_;
  path_ = std::move(other.path_);
  other.db_ = nullptr;
}

Db& Db::operator=(Db&& other) noexcept {
  if (this != &other) {
    close();
    db_ = other.db_;
    path_ = std::move(other.path_);
    other.db_ = nullptr;
  }
  return *this;
}

void Db::throw_sqlite(const std::string& what, int rc, sqlite3* db) {
  std::string msg = what + " (rc=" + std::to_string(rc) + ")";
  if (db) {
    msg += ": ";
    msg += sqlite3_errmsg(db);
  }
  throw std::runtime_error(msg);
}

void Db::open(const std::filesystem::path& path) {
  if (db_) close();

  path_ = path;
  int rc = sqlite3_open_v2(
      path.string().c_str(),
      &db_,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
      nullptr);

  if (rc != SQLITE_OK) {
    // db_ may be non-null even on failure
    throw_sqlite("sqlite open failed: " + path.string(), rc, db_);
  }

  // Sensible defaults for a local app DB.
  exec("PRAGMA foreign_keys = ON;");
  exec("PRAGMA journal_mode = WAL;");
  exec("PRAGMA synchronous = NORMAL;");
  if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK) {
    throw_sqlite("sqlite busy_timeout failed", sqlite3_errcode(db_), db_);
  }
}

void Db::close() {
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

void Db::exec(const std::string& sql) {
  char* err = nullptr;
  int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = err ? err : "unknown sqlite error";
    sqlite3_free(err);
    throw_sqlite("sqlite exec failed: " + msg, rc, db_);
  } // LCOV_EXCL_LINE
}

} // namespace holder::platform
