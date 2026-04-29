#include "platform/Db.h"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace holder::platform {
namespace {

void exec_open_pragma(sqlite3* db, const std::string& sql) {
  constexpr int kMaxAttempts = 50;
  constexpr auto kRetryDelay = std::chrono::milliseconds(10);

  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
      return;
    }

    std::string msg = err ? err : "unknown sqlite error"; // LCOV_EXCL_LINE
    sqlite3_free(err); // LCOV_EXCL_LINE

    if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && attempt + 1 < kMaxAttempts) { // LCOV_EXCL_LINE
      std::this_thread::sleep_for(kRetryDelay); // LCOV_EXCL_LINE
      continue; // LCOV_EXCL_LINE
    }

    std::string full = "sqlite exec failed: " + msg + " (rc=" + std::to_string(rc) + ")"; // LCOV_EXCL_LINE
    if (db) { // LCOV_EXCL_LINE
      full += ": "; // LCOV_EXCL_LINE
      full += sqlite3_errmsg(db); // LCOV_EXCL_LINE
    }
    throw std::runtime_error(full); // LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE
}

} // namespace

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

  if (sqlite3_busy_timeout(db_, 5000) != SQLITE_OK) {
    throw_sqlite("sqlite busy_timeout failed", sqlite3_errcode(db_), db_); // LCOV_EXCL_LINE
  }

  // Sensible defaults for a local app DB.
  exec_open_pragma(db_, "PRAGMA foreign_keys = ON;");
  exec_open_pragma(db_, "PRAGMA journal_mode = WAL;");
  exec_open_pragma(db_, "PRAGMA synchronous = NORMAL;");
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
