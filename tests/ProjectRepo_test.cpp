#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Project.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"

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
  auto dir = base / ("holder_project_test_" + suffix);
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

} // namespace

TEST_CASE("ProjectRepo CRUD", "[projectrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);

  holder::store::ProjectRepo repo(db);

  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Alpha";
  project.root_path = "/tmp/alpha";
  project.created_at = 10;
  project.updated_at = 10;

  repo.create(project);

  const auto fetched = repo.get("proj-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->name == "Alpha");
  REQUIRE(fetched->root_path == "/tmp/alpha");

  auto list = repo.list();
  REQUIRE(list.size() == 1);
  REQUIRE(list[0].project_id == "proj-1");

  repo.update_name("proj-1", "Beta", 20);
  const auto updated_name = repo.get("proj-1");
  REQUIRE(updated_name.has_value());
  REQUIRE(updated_name->name == "Beta");
  REQUIRE(updated_name->updated_at == 20);

  repo.update_root_path("proj-1", "/tmp/beta", 30);
  const auto updated_root = repo.get("proj-1");
  REQUIRE(updated_root.has_value());
  REQUIRE(updated_root->root_path == "/tmp/beta");
  REQUIRE(updated_root->updated_at == 30);

  repo.touch_updated("proj-1", 40);
  const auto touched = repo.get("proj-1");
  REQUIRE(touched.has_value());
  REQUIRE(touched->updated_at == 40);

  repo.remove("proj-1");
  REQUIRE_FALSE(repo.get("proj-1").has_value());
  list = repo.list();
  REQUIRE(list.empty());
}
