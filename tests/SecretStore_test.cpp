#include "ai/AiProviderCredentialRecovery.h"
#include "ai/AiProviderCredentialRepo.h"
#include "http_test_helpers.h"
#include "privacy/SecretStore.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("SecretStore set/get/list/remove persists through index-backed test store", "[privacy]") {
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  store->set("holder.ai_provider_credentials",
             "chocolatefactory",
             "cf_test_secret",
             "cf_****cret",
             100,
             120);

  const auto item = store->get("holder.ai_provider_credentials", "chocolatefactory");
  REQUIRE(item.has_value());
  REQUIRE(item->secret == "cf_test_secret");
  REQUIRE(item->metadata.preview == "cf_****cret");
  REQUIRE(item->metadata.created_at == 100);
  REQUIRE(item->metadata.updated_at == 120);

  const auto listed = store->list("holder.ai_provider_credentials");
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].account == "chocolatefactory");
  REQUIRE(listed[0].preview == "cf_****cret");

  auto store_again = holder::privacy::make_default_secret_store(dir / "server");
  const auto item_again = store_again->get("holder.ai_provider_credentials", "chocolatefactory");
  REQUIRE(item_again.has_value());
  REQUIRE(item_again->secret == "cf_test_secret");

  store_again->remove("holder.ai_provider_credentials", "chocolatefactory");
  REQUIRE_FALSE(store_again->get("holder.ai_provider_credentials", "chocolatefactory").has_value());
  REQUIRE(store_again->list("holder.ai_provider_credentials").empty());
}

TEST_CASE("AI provider credential metadata rebuilds from SecretStore after sqlite deletion", "[startup][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

  auto store = holder::privacy::make_default_secret_store(dir / "server");
  store->set("holder.ai_provider_credentials",
             "switchyard",
             "sw_secret_123",
             "sw_****123",
             200,
             240);

  const auto db_path = dir / "holder.db";
  holder::platform::Db db;
  db.open(db_path);
  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  holder::ai::recover_ai_provider_credentials_from_secret_store(db, *store);

  holder::ai::AiProviderCredentialRepo repo(db);
  const auto row = repo.get("switchyard");
  REQUIRE(row.has_value());
  REQUIRE(row->api_key_preview == "sw_****123");
  REQUIRE(row->created_at == 200);
  REQUIRE(row->updated_at == 240);
}
