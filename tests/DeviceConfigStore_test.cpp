#include "ai/AiLocalModelConfigRepo.h"
#include "ai/AiProviderSettingRepo.h"
#include "ai/AiRunnerRepo.h"
#include "http_test_helpers.h"
#include "platform/DeviceConfigStore.h"

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include <filesystem>

TEST_CASE("DeviceConfigStore restores daemon settings into fresh SQLite", "[database][config]") {
  const auto dir = holder::test::make_temp_dir();
  const auto config_path = dir / "config" / "daemon-config.json";
  auto original = holder::test::open_db_with_schema(dir / "original.db");
  holder::ai::AiLocalModelConfigRepo(original).set("ollama:fast", std::nullopt, "ollama:deep", 10);

  holder::model::AiRunner runner;
  runner.runner_id = "manual-one";
  runner.name = "Office runner";
  runner.kind = "ollama";
  runner.base_url = "http://127.0.0.1:11434";
  runner.source = "manual";
  runner.enabled = true;
  runner.created_at = 11;
  runner.updated_at = 12;
  holder::ai::AiRunnerRepo(original).upsert(runner);
  holder::ai::AiProviderSettingRepo(original).upsert("provider-one", true, 13);
  const auto expected_local = holder::ai::AiLocalModelConfigRepo(original).get();
  REQUIRE(expected_local.has_value());

  holder::core::initialize_device_config(original, config_path);
  REQUIRE(std::filesystem::is_regular_file(config_path));

  auto rebuilt = holder::test::open_db_with_schema(dir / "rebuilt.db");
  holder::core::restore_device_config(rebuilt, config_path);
  const auto restored_local = holder::ai::AiLocalModelConfigRepo(rebuilt).get();
  REQUIRE(restored_local.has_value());
  REQUIRE(restored_local->fast_model == expected_local->fast_model);
  REQUIRE(restored_local->deep_model == expected_local->deep_model);
  const auto restored_runner = holder::ai::AiRunnerRepo(rebuilt).get(runner.runner_id);
  REQUIRE(restored_runner.has_value());
  REQUIRE(restored_runner->name == runner.name);
  const auto restored_provider = holder::ai::AiProviderSettingRepo(rebuilt).get("provider-one");
  REQUIRE(restored_provider.has_value());
  REQUIRE(restored_provider->enabled);
}

TEST_CASE("DeviceConfigStore persists later route-style mutations", "[database][config]") {
  const auto dir = holder::test::make_temp_dir();
  const auto config_path = dir / "daemon-config.json";
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::core::initialize_device_config(db, config_path);
  holder::ai::AiProviderSettingRepo(db).upsert("changed", true, 20);
  holder::core::persist_device_config(db);

  auto rebuilt = holder::test::open_db_with_schema(dir / "fresh.db");
  holder::core::restore_device_config(rebuilt, config_path);
  REQUIRE(holder::ai::AiProviderSettingRepo(rebuilt).get("changed")->enabled);
}

TEST_CASE(
    "DeviceConfigStore replaces manual settings and rejects unsupported versions",
    "[database][config]"
) {
  const auto root = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(root / "holder.db");
  const auto path = root / "config.json";
  holder::model::AiRunner runner;
  runner.runner_id = "manual";
  runner.name = "Manual";
  runner.kind = "ollama";
  runner.source = "manual";
  runner.created_at = runner.updated_at = 1;
  holder::ai::AiRunnerRepo(db).upsert(runner);
  holder::ai::AiProviderSettingRepo(db).upsert("provider", true, 1);
  std::ofstream(path) << R"({"version":2})";
  REQUIRE_THROWS(holder::core::restore_device_config(db, path));
  std::ofstream(path) << R"({"version":1,"manual_runners":[],"provider_settings":[]})";
  holder::core::restore_device_config(db, path);
  CHECK_FALSE(holder::ai::AiRunnerRepo(db).get("manual").has_value());
  CHECK_FALSE(holder::ai::AiProviderSettingRepo(db).get("provider").has_value());
}
