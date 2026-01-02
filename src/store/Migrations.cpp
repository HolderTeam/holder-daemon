#include "store/Migrations.h"
#include "store/Tx.h"

#include <spdlog/spdlog.h>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace holder::store {

std::string Migrations::read_file(const std::filesystem::path& p) {
  std::ifstream in(p);
  if (!in.is_open()) {
    throw std::runtime_error("Failed to open schema file: " + p.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool Migrations::has_any_tables(Db& db) {
  // Check for any non-internal tables.
  static constexpr const char* SQL =
      "SELECT 1 "
      "FROM sqlite_master "
      "WHERE type='table' "
      "  AND name NOT LIKE 'sqlite_%' "
      "LIMIT 1;";

  sqlite3_stmt* stmt = nullptr;
  int rc = sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    throw std::runtime_error(std::string("sqlite prepare failed: ") + sqlite3_errmsg(db.handle()));
  }

  rc = sqlite3_step(stmt);
  bool any = (rc == SQLITE_ROW);

  sqlite3_finalize(stmt);
  return any;
}

void Migrations::ensure_schema(Db& db, const std::filesystem::path& schema_sql_path) {
  spdlog::info("DB opened: {}", db.path().string());

  if (has_any_tables(db)) {
    spdlog::info("Schema already present (tables exist), skipping schema apply.");
    return;
  }

  spdlog::info("No tables found; applying schema from: {}", schema_sql_path.string());
  const auto sql = read_file(schema_sql_path);

  Tx tx(db);
  db.exec(sql);
  tx.commit();

  spdlog::info("Schema applied successfully.");
}

} // namespace holder::store
