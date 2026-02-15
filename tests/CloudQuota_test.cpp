#include "api/support/CloudQuota.h"
#include "store/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("CloudQuota cooldown failure backoff and clear", "[cloud_quota]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_cloud_quota";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

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

  holder::store::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

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
