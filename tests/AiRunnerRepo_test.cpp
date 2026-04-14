#include "ai/AiRunnerRepo.h"
#include "http_test_helpers.h"
#include "llm/LocalRunnerClient.h"
#include "llm/LocalModelRunner.h"
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

  const auto auto_local = registry.get_runner("auto-local");
  REQUIRE(auto_local.has_value());
  REQUIRE(auto_local->source == "auto_local");
}

TEST_CASE("RunnerRegistry get_runner returns nullopt without backing db", "[db]") {
  holder::llm::RunnerRegistry registry(nullptr, nullptr);
  REQUIRE_FALSE(registry.get_runner("manual-a").has_value());
}

TEST_CASE("RunnerRegistry instantiates manual runner clients for enabled ollama runners", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_runner_registry_clients";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  holder::test::EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "1");

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
  repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-disabled",
      .name = "Disabled Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://disabled:11434"),
      .source = "manual",
      .enabled = false,
      .created_at = 101,
      .updated_at = 101,
  });
  repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-bad",
      .name = "Bad Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("https://bad.example"),
      .source = "manual",
      .enabled = true,
      .created_at = 102,
      .updated_at = 102,
  });
  repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-empty-host",
      .name = "Empty Host",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://:11434"),
      .source = "manual",
      .enabled = true,
      .created_at = 103,
      .updated_at = 103,
  });
  repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-empty-port",
      .name = "Empty Port",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://host:"),
      .source = "manual",
      .enabled = true,
      .created_at = 104,
      .updated_at = 104,
  });
  repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-path",
      .name = "Path Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://path-host:11434/api"),
      .source = "manual",
      .enabled = true,
      .created_at = 105,
      .updated_at = 105,
  });
  repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-slash-only",
      .name = "Slash Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http:///"),
      .source = "manual",
      .enabled = true,
      .created_at = 106,
      .updated_at = 106,
  });

  holder::llm::LocalModelRunner auto_local_runner;
  holder::llm::LocalRunnerClient auto_local_client(&auto_local_runner);
  holder::llm::RunnerRegistry registry(&db, &auto_local_client);

  auto* manual_client = registry.get_client("manual-a");
  REQUIRE(manual_client != nullptr);
  const auto status = manual_client->retry();
  REQUIRE(status.available == true);
  REQUIRE(status.version == "fake");
  REQUIRE(status.models.size() == 1);
  REQUIRE(status.models[0].name == "fake-echo");

  REQUIRE(registry.get_client("manual-disabled") == nullptr);
  REQUIRE(registry.get_client("manual-bad") == nullptr);
  REQUIRE(registry.get_client("manual-empty-host") == nullptr);
  REQUIRE(registry.get_client("manual-empty-port") == nullptr);
  REQUIRE(registry.get_client("manual-slash-only") == nullptr);

  auto* path_client = registry.get_client("manual-path");
  REQUIRE(path_client != nullptr);
  const auto path_status = path_client->retry();
  REQUIRE(path_status.available == true);
  REQUIRE(path_status.version == "fake");
  REQUIRE(path_status.models.size() == 1);
  REQUIRE(path_status.models[0].name == "fake-echo");
}
