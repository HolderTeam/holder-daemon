#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Project.h"
#include "model/Resource.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"
#include "store/ResourceRepo.h"

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
  auto dir = base / ("holder_resource_test_" + suffix);
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

void create_project(holder::store::Db& db, const std::string& project_id) {
  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

} // namespace

TEST_CASE("ResourceRepo add/list/remove", "[resourcerepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::store::ResourceRepo repo(db);

  holder::model::Resource resource;
  resource.resource_id = "res-1";
  resource.project_id = "proj-1";
  resource.kind = "url";
  resource.uri = "https://example.com";
  resource.label = "Example";
  resource.desc = "Docs";
  resource.created_at = 10;
  resource.updated_at = 10;

  repo.add(resource);

  auto list = repo.list("proj-1");
  REQUIRE(list.size() == 1);
  REQUIRE(list[0].resource_id == "res-1");
  REQUIRE(list[0].desc.has_value());

  const auto fetched = repo.get("res-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->uri == "https://example.com");

  repo.remove("res-1");
  list = repo.list("proj-1");
  REQUIRE(list.empty());
}
