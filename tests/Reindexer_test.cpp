#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "index/Reindexer.h"
#include "store/Db.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path p = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(p)) return p;
#endif
  namespace fs = std::filesystem;
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_reindex_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::store::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

int count_table(holder::store::Db& db, const std::string& table) {
  const std::string sql = "SELECT count(*) FROM " + table + ";";
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

} // namespace

TEST_CASE("Reindexer rebuilds FTS tables", "[reindex]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
          "VALUES('card-1', 'proj-1', 'Title', 'cards/aa/bb/card-1.md', 0.0, 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 2, 2);");
  db.exec("INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
          "VALUES('msg-1', 'thread-1', 'user', 'manual', 'Hello', 3);");

  db.exec("DELETE FROM cards_fts;");
  db.exec("DELETE FROM ai_fts;");

  holder::index::Reindexer reindexer(db);
  reindexer.run();

  REQUIRE(count_table(db, "cards_fts") == 1);
  REQUIRE(count_table(db, "ai_fts") == 1);
}
