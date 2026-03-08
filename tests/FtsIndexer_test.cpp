#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "index/FtsIndexer.h"
#include "platform/Db.h"

#include <chrono>
#include <cmath>
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
  auto dir = base / ("holder_fts_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

int count_cards_fts(holder::platform::Db& db, const std::string& query) {
  static constexpr const char* SQL =
      "SELECT count(*) FROM cards_fts WHERE cards_fts MATCH ?;";
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

int count_ai_fts(holder::platform::Db& db, const std::string& query) {
  static constexpr const char* SQL =
      "SELECT count(*) FROM ai_fts WHERE ai_fts MATCH ?;";
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_bind_text(stmt, 1, query.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

} // namespace

TEST_CASE("FtsIndexer card upsert/delete", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  fts.upsert_card("card-1", "proj-1", "Title", "Hello world");
  REQUIRE(count_cards_fts(db, "hello") == 1);

  fts.upsert_card("card-1", "proj-1", "Title", "Goodbye world");
  REQUIRE(count_cards_fts(db, "hello") == 0);
  REQUIRE(count_cards_fts(db, "goodbye") == 1);

  fts.delete_card("card-1");
  REQUIRE(count_cards_fts(db, "goodbye") == 0);
}

TEST_CASE("FtsIndexer message upsert/delete", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  fts.upsert_message("msg-1", "thread-1", "proj-1", "alpha beta");
  REQUIRE(count_ai_fts(db, "alpha") == 1);

  fts.upsert_message("msg-1", "thread-1", "proj-1", "gamma");
  REQUIRE(count_ai_fts(db, "alpha") == 0);
  REQUIRE(count_ai_fts(db, "gamma") == 1);

  fts.delete_message("msg-1");
  REQUIRE(count_ai_fts(db, "gamma") == 0);
}

TEST_CASE("FtsIndexer search returns snippets", "[fts]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::index::FtsIndexer fts(db);
  {
    static constexpr const char* SQL_PROJECT =
        "INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
        "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);";
    db.exec(SQL_PROJECT);
    static constexpr const char* SQL_CARD =
        "INSERT INTO cards(card_id, project_id, title, rel_path, sort_key, created_at, updated_at) "
        "VALUES('card-1', 'proj-1', 'Title', 'cards/xx/yy/card-1.md', 0.0, 1, 2);";
    db.exec(SQL_CARD);
  }
  fts.upsert_card("card-1", "proj-1", "Title", "The quick brown fox");
  const auto rows = fts.search_cards("proj-1", "brown", 10, 0);
  REQUIRE(rows.size() == 1);
  REQUIRE(rows[0].id == "card-1");
  REQUIRE(rows[0].title == "Title");
  REQUIRE(rows[0].created_at == 1);
  REQUIRE(rows[0].updated_at == 2);
  REQUIRE(std::isfinite(rows[0].rank));
  REQUIRE(rows[0].snippet.find('[') != std::string::npos);
}
