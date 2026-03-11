#include "api/support/CloudQuota.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

void apply_schema(holder::platform::Db& db) {
  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

} // namespace

TEST_CASE("CloudQuota cooldown failure backoff and clear", "[cloud_quota]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_quota";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  const std::string provider = "chocolatefactory";
  const std::string model = "gemma-3-12b-it";

  const auto missing = holder::api::support::load_cloud_model_cooldown(db, provider, model);
  REQUIRE_FALSE(missing.has_value());

  const auto first =
      holder::api::support::record_cloud_model_failure(db, provider, model, "timeout", 1000);
  REQUIRE(first.failure_count == 1);
  REQUIRE(first.cooldown_until == 1030);

  const auto second =
      holder::api::support::record_cloud_model_failure(db, provider, model, "429", 1050);
  REQUIRE(second.failure_count == 2);
  REQUIRE(second.cooldown_until == 1110);

  const auto loaded = holder::api::support::load_cloud_model_cooldown(db, provider, model);
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->failure_count == 2);
  REQUIRE(loaded->cooldown_until == 1110);
  REQUIRE(loaded->last_error == "429");

  holder::api::support::clear_cloud_model_cooldown(db, provider, model, 1200);
  const auto cleared = holder::api::support::load_cloud_model_cooldown(db, provider, model);
  REQUIRE(cleared.has_value());
  REQUIRE(cleared->failure_count == 0);
  REQUIRE(cleared->cooldown_until == 0);
  REQUIRE(cleared->last_error.empty());
  REQUIRE(cleared->updated_at == 1200);
}

TEST_CASE("CloudQuota cooldown supports configurable base/cap", "[cloud_quota]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_quota_custom";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  const std::string provider = "chocolatefactory";
  const std::string model = "gemma-3-1b-it";

  const auto first = holder::api::support::record_cloud_model_failure(
      db, provider, model, "timeout", 1000, 10, 40);
  REQUIRE(first.failure_count == 1);
  REQUIRE(first.cooldown_until == 1010);

  const auto second = holder::api::support::record_cloud_model_failure(
      db, provider, model, "timeout", 1010, 10, 40);
  REQUIRE(second.failure_count == 2);
  REQUIRE(second.cooldown_until == 1030);

  const auto third = holder::api::support::record_cloud_model_failure(
      db, provider, model, "timeout", 1020, 10, 40);
  REQUIRE(third.failure_count == 3);
  REQUIRE(third.cooldown_until == 1060); // 40s cap
}

TEST_CASE("CloudQuota prepare-query errors are surfaced", "[cloud_quota]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_quota_prepare_errors";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  db.close();

  REQUIRE_THROWS_WITH(holder::api::support::load_cloud_window_usage(db, "p", "m", 0),
                      Catch::Matchers::ContainsSubstring("prepare cloud usage query failed"));
  REQUIRE_THROWS_WITH(holder::api::support::record_cloud_usage_event(db, "p", "m", 1, 2, 3, "seed"),
                      Catch::Matchers::ContainsSubstring("prepare cloud usage insert failed"));
  REQUIRE_THROWS_WITH(holder::api::support::load_cloud_model_cooldown(db, "p", "m"),
                      Catch::Matchers::ContainsSubstring("prepare cloud cooldown query failed"));
}

TEST_CASE("CloudQuota insert/upsert/clear step failures are surfaced", "[cloud_quota]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_quota_step_errors";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);

  db.exec(
      "CREATE TRIGGER fail_cloud_usage_insert "
      "BEFORE INSERT ON ai_cloud_usage_events "
      "BEGIN "
      "  SELECT RAISE(ABORT, 'fail usage insert'); "
      "END;");
  REQUIRE_THROWS_WITH(
      holder::api::support::record_cloud_usage_event(db, "p", "m", 1, 2, 3, "seed"),
      Catch::Matchers::ContainsSubstring("insert cloud usage event failed"));
  db.exec("DROP TRIGGER fail_cloud_usage_insert;");

  db.exec(
      "CREATE TRIGGER fail_cloud_cooldown_insert "
      "BEFORE INSERT ON ai_cloud_model_cooldowns "
      "BEGIN "
      "  SELECT RAISE(ABORT, 'fail cooldown upsert'); "
      "END;");
  REQUIRE_THROWS_WITH(
      holder::api::support::record_cloud_model_failure(db, "p", "m", "oops", 1000),
      Catch::Matchers::ContainsSubstring("upsert cloud cooldown failed"));
  REQUIRE_THROWS_WITH(
      holder::api::support::clear_cloud_model_cooldown(db, "p", "m", 1100),
      Catch::Matchers::ContainsSubstring("clear cloud cooldown failed"));
}
