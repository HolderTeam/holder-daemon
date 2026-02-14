#include "store/AiRouterConfigRepo.h"
#include "store/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("AiRouterConfigRepo global and project config", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_router_config";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::store::AiRouterConfigRepo repo(db);

  REQUIRE_FALSE(repo.get_global().has_value());
  REQUIRE_FALSE(repo.get_for_project("proj-1").has_value());

  repo.set_global("qwen2.5:0.5b", 100);
  auto global = repo.get_global();
  REQUIRE(global.has_value());
  REQUIRE(global->scope == "global");
  REQUIRE(global->router_model == "qwen2.5:0.5b");

  repo.set_for_project("proj-1", "deepseek-r1:latest", 200);
  auto project = repo.get_for_project("proj-1");
  REQUIRE(project.has_value());
  REQUIRE(project->scope == "project");
  REQUIRE(project->project_id.value() == "proj-1");
  REQUIRE(project->router_model == "deepseek-r1:latest");

  repo.clear_for_project("proj-1");
  REQUIRE_FALSE(repo.get_for_project("proj-1").has_value());

  repo.clear_global();
  REQUIRE_FALSE(repo.get_global().has_value());
}
