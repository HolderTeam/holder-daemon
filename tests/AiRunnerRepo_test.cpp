#include "ai/AiRunnerRepo.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("AiRunnerRepo stores and removes manual runners", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runner_repo";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  holder::platform::Db db;
  db.open(dir / "holder.db");

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiRunnerRepo repo(db);

  holder::model::AiRunner runner{
      .runner_id = "manual-test",
      .name = "Test Runner",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://10.0.0.5:11434"),
      .source = "manual",
      .enabled = true,
      .created_at = 10,
      .updated_at = 10,
  };

  repo.upsert(runner);
  auto stored = repo.get("manual-test");
  REQUIRE(stored.has_value());
  REQUIRE(stored->name == "Test Runner");
  REQUIRE(stored->base_url == std::optional<std::string>("http://10.0.0.5:11434"));

  runner.name = "Test Runner 2";
  runner.enabled = false;
  runner.updated_at = 20;
  repo.upsert(runner);

  stored = repo.get("manual-test");
  REQUIRE(stored.has_value());
  REQUIRE(stored->name == "Test Runner 2");
  REQUIRE(stored->enabled == false);
  REQUIRE(stored->created_at == 10);
  REQUIRE(stored->updated_at == 20);

  repo.remove("manual-test");
  REQUIRE_FALSE(repo.get("manual-test").has_value());
}

TEST_CASE("RunnerRegistry merges auto-local and persisted manual runners", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_runner_registry_repo";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  holder::platform::Db db;
  db.open(dir / "holder.db");

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiRunnerRepo repo(db);
  repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-a",
      .name = "Office Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://office:11434"),
      .source = "manual",
      .enabled = true,
      .created_at = 100,
      .updated_at = 100,
  });

  holder::llm::RunnerRegistry registry(&db, nullptr);
  const auto runners = registry.list_runners();

  REQUIRE(runners.size() == 2);
  REQUIRE(runners[0].runner_id == std::string("auto-local"));
  REQUIRE(runners[1].runner_id == std::string("manual-a"));

  const auto manual = registry.get_runner("manual-a");
  REQUIRE(manual.has_value());
  REQUIRE(manual->name == "Office Ollama");
  REQUIRE(manual->source == "manual");
}
