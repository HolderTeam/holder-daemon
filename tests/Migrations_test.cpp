#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "platform/Db.h"
#include "platform/Migrations.h"

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
  auto dir = base / ("holder_migrations_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("ensure_schema_version accepts expected version", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  const auto schema_path = find_schema_sql();
  holder::platform::Migrations::ensure_schema(db, schema_path);

  REQUIRE_NOTHROW(holder::platform::Migrations::ensure_schema_version(db, 1));
}

TEST_CASE("ensure_schema_version rejects mismatch", "[migrations]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  const auto schema_path = find_schema_sql();
  holder::platform::Migrations::ensure_schema(db, schema_path);

  REQUIRE_THROWS_WITH(holder::platform::Migrations::ensure_schema_version(db, 2),
                      Catch::Matchers::ContainsSubstring("Schema version mismatch"));
}
