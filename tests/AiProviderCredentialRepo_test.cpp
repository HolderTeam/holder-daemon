#include "ai/AiProviderCredentialRepo.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("AiProviderCredentialRepo upsert/list/remove", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_provider_credentials";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::AiProviderCredentialRepo repo(db);
  REQUIRE(repo.list().empty());

  repo.upsert("chocolatefactory", "key-1", 100, 100);
  auto one = repo.get("chocolatefactory");
  REQUIRE(one.has_value());
  REQUIRE(one->provider == "chocolatefactory");
  REQUIRE(one->api_key == "key-1");
  REQUIRE(one->created_at == 100);
  REQUIRE(one->updated_at == 100);

  repo.upsert("chocolatefactory", "key-2", 999, 200);
  one = repo.get("chocolatefactory");
  REQUIRE(one.has_value());
  REQUIRE(one->api_key == "key-2");
  REQUIRE(one->created_at == 100);
  REQUIRE(one->updated_at == 200);

  repo.upsert("openai", "other", 300, 300);
  const auto rows = repo.list();
  REQUIRE(rows.size() == 2);

  repo.remove("chocolatefactory");
  REQUIRE_FALSE(repo.get("chocolatefactory").has_value());
}
