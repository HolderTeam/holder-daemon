#include "ai/AiLocalModelConfigRepo.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sqlite3.h>

namespace {
int sqlite_interrupt_cb(void*) { return 1; }
} // namespace

TEST_CASE("AiLocalModelConfigRepo stores and clears config", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_local_model_config";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiLocalModelConfigRepo repo(db);

  REQUIRE_FALSE(repo.get().has_value());

  repo.set(std::string("fast"), std::string("strong"), std::string("deep"), 100);
  auto cfg = repo.get();
  REQUIRE(cfg.has_value());
  REQUIRE(cfg->fast_model == std::optional<std::string>("auto-local::fast"));
  REQUIRE(cfg->strong_model == std::optional<std::string>("auto-local::strong"));
  REQUIRE(cfg->deep_model == std::optional<std::string>("auto-local::deep"));
  REQUIRE(cfg->updated_at == 100);

  repo.set(std::string("fast-2"), std::nullopt, std::string("deep-2"), 200);
  cfg = repo.get();
  REQUIRE(cfg.has_value());
  REQUIRE(cfg->fast_model == std::optional<std::string>("auto-local::fast-2"));
  REQUIRE_FALSE(cfg->strong_model.has_value());
  REQUIRE(cfg->deep_model == std::optional<std::string>("auto-local::deep-2"));
  REQUIRE(cfg->updated_at == 200);

  repo.clear();
  REQUIRE_FALSE(repo.get().has_value());
}

TEST_CASE("AiLocalModelConfigRepo clears when all models are empty", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_local_model_config_clear_empty";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiLocalModelConfigRepo repo(db);
  repo.set(std::string("fast"), std::string("strong"), std::string("deep"), 1);
  REQUIRE(repo.get().has_value());

  repo.set(std::nullopt, std::nullopt, std::nullopt, 2);
  REQUIRE_FALSE(repo.get().has_value());
}

TEST_CASE("AiLocalModelConfigRepo throws when get prepare fails", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_local_model_config_get_prepare_fail";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("DROP TABLE ai_local_model_config;");

  holder::ai::AiLocalModelConfigRepo repo(db);
  REQUIRE_THROWS(repo.get());
}

TEST_CASE("AiLocalModelConfigRepo normalizes legacy bare model refs on read", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_local_model_config_legacy";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  db.exec(
      "INSERT INTO ai_local_model_config(key, fast_model, strong_model, deep_model, updated_at) "
      "VALUES('global', 'legacy-fast', 'legacy-strong', NULL, 7);"
  );

  holder::ai::AiLocalModelConfigRepo repo(db);
  const auto cfg = repo.get();
  REQUIRE(cfg.has_value());
  REQUIRE(cfg->fast_model == std::optional<std::string>("auto-local::legacy-fast"));
  REQUIRE(cfg->strong_model == std::optional<std::string>("auto-local::legacy-strong"));
  REQUIRE_FALSE(cfg->deep_model.has_value());
}

TEST_CASE("AiLocalModelConfigRepo throws when get step is interrupted", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() /
                   "holder_ai_local_model_config_get_interrupt";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiLocalModelConfigRepo repo(db);
  repo.set(std::string("fast"), std::nullopt, std::nullopt, 1);

  sqlite3_progress_handler(db.handle(), 1, sqlite_interrupt_cb, nullptr);
  REQUIRE_THROWS(repo.get());
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("AiLocalModelConfigRepo throws on set and clear failures", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_local_model_config_failures";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiLocalModelConfigRepo repo(db);
  db.exec("CREATE TRIGGER fail_local_model_insert BEFORE INSERT ON ai_local_model_config "
          "BEGIN SELECT RAISE(ABORT, 'no insert'); END;");
  REQUIRE_THROWS(repo.set(std::string("fast"), std::nullopt, std::nullopt, 1));

  db.exec("DROP TRIGGER fail_local_model_insert;");
  repo.set(std::string("fast"), std::nullopt, std::nullopt, 1);
  db.exec("CREATE TRIGGER fail_local_model_delete BEFORE DELETE ON ai_local_model_config "
          "BEGIN SELECT RAISE(ABORT, 'no delete'); END;");
  REQUIRE_THROWS(repo.clear());
}
