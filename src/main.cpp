#include <spdlog/spdlog.h>

#include "core/Paths.h"
#include "store/Db.h"
#include "store/Migrations.h"

#include <filesystem>

static std::filesystem::path find_schema_sql() {
  namespace fs = std::filesystem;

  // Dev-friendly: run from repo root
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;

  // Or if run from build/ directory
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;

  throw std::runtime_error("Cannot find schema/schema.sql from current directory.");
}

int main() {
  spdlog::info("holder starting…");

  auto paths = holder::core::Paths::resolve("holder");
  paths.ensure_dirs();

  spdlog::info("data_dir:   {}", paths.data_dir.string());
  spdlog::info("db_path:    {}", paths.db_path().string());

  holder::store::Db db;
  db.open(paths.db_path());

  const auto schema_path = find_schema_sql();
  holder::store::Migrations::ensure_schema(db, schema_path);

  spdlog::info("holder boot complete.");
  return 0;
}
