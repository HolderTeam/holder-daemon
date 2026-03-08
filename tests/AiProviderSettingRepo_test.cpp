#include "ai/AiProviderSettingRepo.h"
#include "store/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("AiProviderSettingRepo upsert/list/remove", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_provider_settings";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderSettingRepo repo(db);
  REQUIRE(repo.list().empty());

  repo.upsert("switchyard", true, 100);
  auto one = repo.get("switchyard");
  REQUIRE(one.has_value());
  REQUIRE(one->provider == "switchyard");
  REQUIRE(one->enabled == true);
  REQUIRE(one->updated_at == 100);

  repo.upsert("switchyard", false, 200);
  one = repo.get("switchyard");
  REQUIRE(one.has_value());
  REQUIRE(one->enabled == false);
  REQUIRE(one->updated_at == 200);

  repo.upsert("chadjeopardy", true, 300);
  const auto rows = repo.list();
  REQUIRE(rows.size() == 2);

  repo.remove("switchyard");
  REQUIRE_FALSE(repo.get("switchyard").has_value());
}
