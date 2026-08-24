#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "api/support/CloudQuota.h"
#include "api/support/ThreadCompaction.h"
#include "ai/AiNudgeRepo.h"
#include "ai/AiThreadDurability.h"
#include "ai/AiThreadRepo.h"
#include "http_test_helpers.h"
#include "platform/DatabaseRecovery.h"
#include "platform/DeviceConfigStore.h"
#include "platform/Migrations.h"
#include "platform/ProjectRegistry.h"
#include "privacy/SecretStore.h"
#include "project/ProjectRepo.h"
#include "project/ProjectStore.h"

#include <filesystem>
#include <fstream>

TEST_CASE("DatabaseRecovery rebuilds a fresh projection and retains backup", "[database][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  holder::core::Paths paths;
  paths.data_dir = dir / "data";
  paths.config_dir = dir / "config";
  paths.cache_dir = dir / "cache";
  paths.ensure_dirs();
  const auto projects_root = paths.data_dir / "projects";
  holder::test::EnvGuard projects_env("HOLDER_PROJECTS_ROOT", projects_root.string());
  holder::test::EnvGuard keys_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keys").string());

  holder::platform::Db original;
  original.open(paths.db_path());
  holder::platform::Migrations::ensure_schema(original, SCHEMA_SQL_PATH);
  holder::project::ProjectStore projects(original);
  holder::model::Project input;
  input.project_id = "project-recovery";
  input.name = "Recovery";
  input.git_remote_url = "https://example.com/holder/recovery.git";
  input.git_provider = "github";
  input.privacy_mode = "plain";
  input.created_at = 10;
  input.updated_at = 10;
  const auto project = projects.create(
      input, [] { return std::string("unused-id"); }, projects_root
  );

  holder::card::CardStore cards(original, nullptr);
  holder::model::Card card;
  card.card_id = "card-recovery";
  card.project_id = project.project_id;
  card.title = "Survives";
  card.created_at = 20;
  card.updated_at = 20;
  cards.create(card, "# Survives\n\nDatabase corruption.\n");
  holder::core::ProjectRegistry(paths.project_registry_path()).remember({project});
  holder::core::initialize_device_config(original, paths.device_config_path());
  holder::api::support::initialize_cloud_usage_ledger(
      original, paths.cloud_usage_ledger_path()
  );
  original.close();

  auto secrets = holder::privacy::make_default_secret_store(paths.server_dir());
  const auto dry_run = holder::core::rebuild_database(paths, SCHEMA_SQL_PATH, *secrets, true);
  REQUIRE(dry_run.dry_run);
  REQUIRE(dry_run.projects == 1);
  REQUIRE(dry_run.cards == 1);
  REQUIRE(dry_run.backup_path.empty());
  REQUIRE(holder::core::inspect_database_health(paths.db_path()).health ==
          holder::core::DatabaseHealth::Healthy);

  const auto report = holder::core::rebuild_database(paths, SCHEMA_SQL_PATH, *secrets, false);
  REQUIRE(report.projects == 1);
  REQUIRE(report.cards == 1);
  REQUIRE_FALSE(report.backup_path.empty());
  REQUIRE(std::filesystem::exists(report.backup_path / "holder.db"));
  REQUIRE(holder::core::inspect_database_health(paths.db_path()).health ==
          holder::core::DatabaseHealth::Healthy);

  holder::platform::Db rebuilt;
  rebuilt.open(paths.db_path());
  const auto recovered_project = holder::project::ProjectRepo(rebuilt).get(project.project_id);
  REQUIRE(recovered_project.has_value());
  REQUIRE(recovered_project->name == project.name);
  REQUIRE(recovered_project->git_remote_url == project.git_remote_url);
  REQUIRE(recovered_project->git_provider == project.git_provider);
  const auto recovered_card = holder::card::CardRepo(rebuilt).get(card.card_id);
  REQUIRE(recovered_card.has_value());
  REQUIRE(recovered_card->project_id == project.project_id);
}

TEST_CASE("DatabaseRecovery quarantines corrupt SQLite and rebuilds from durable owners", "[database][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  holder::core::Paths paths;
  paths.data_dir = dir / "data";
  paths.config_dir = dir / "config";
  paths.cache_dir = dir / "cache";
  paths.ensure_dirs();
  const auto projects_root = paths.data_dir / "projects";
  holder::test::EnvGuard projects_env("HOLDER_PROJECTS_ROOT", projects_root.string());
  holder::test::EnvGuard keys_env("HOLDER_TEST_KEYSTORE_DIR", (dir / "keys").string());

  holder::platform::Db db;
  db.open(paths.db_path());
  holder::platform::Migrations::ensure_schema(db, SCHEMA_SQL_PATH);
  holder::model::Project input;
  input.project_id = "project-corrupt";
  input.name = "Corrupt recovery";
  input.privacy_mode = "plain";
  input.created_at = 10;
  input.updated_at = 10;
  const auto project = holder::project::ProjectStore(db).create(
      input, [] { return std::string("unused"); }, projects_root
  );
  holder::model::Card card;
  card.card_id = "card-corrupt";
  card.project_id = project.project_id;
  card.title = "Still here";
  card.created_at = 20;
  card.updated_at = 20;
  holder::card::CardStore(db, nullptr).create(card, "# Still here\n");
  holder::core::ProjectRegistry(paths.project_registry_path()).remember({project});
  holder::core::initialize_device_config(db, paths.device_config_path());
  holder::api::support::initialize_cloud_usage_ledger(db, paths.cloud_usage_ledger_path());
  holder::api::support::record_cloud_usage_event(
      db, "provider", "model", 7, 5, 30, "corrupt-recovery"
  );

  holder::model::AiThread thread;
  thread.thread_id = "thread-corrupt";
  thread.project_id = project.project_id;
  thread.card_id = card.card_id;
  thread.title = "Durable conversation";
  thread.created_at = 21;
  thread.updated_at = 22;
  holder::ai::AiThreadRepo(db).create(thread);
  holder::ai::persist_ai_thread(db, thread);
  holder::api::support::ThreadCompactionState compaction;
  compaction.thread_id = thread.thread_id;
  compaction.rolling_summary = "A durable summary";
  compaction.pinned_facts_json = R"(["Never forget this"] )";
  compaction.updated_at = 23;
  holder::api::support::upsert_thread_compaction_state(db, compaction);

  holder::ai::AiNudgeRepo nudges(db);
  nudges.create({
      .nudge_id = "nudge-corrupt",
      .kind = "card.title_only",
      .project_id = project.project_id,
      .card_id = card.card_id,
      .title = "Dismiss me",
      .body = "This must stay dismissed.",
      .created_at = 24,
  });
  REQUIRE(nudges.dismiss("nudge-corrupt"));
  holder::core::audit_durable_database_ownership(db, paths);
  holder::core::mark_database_rebuild_ready(paths);
  db.close();

  {
    std::ofstream damaged(paths.db_path(), std::ios::binary | std::ios::trunc);
    damaged << "not a sqlite database";
  }
  REQUIRE(holder::core::inspect_database_health(paths.db_path()).health ==
          holder::core::DatabaseHealth::Corrupt);

  auto secrets = holder::privacy::make_default_secret_store(paths.server_dir());
  const auto report = holder::core::rebuild_database(paths, SCHEMA_SQL_PATH, *secrets, false);
  REQUIRE(report.previous_health == "corrupt");
  REQUIRE(report.cards == 1);
  REQUIRE(report.backup_path.filename().string().starts_with("quarantine-"));
  REQUIRE(std::filesystem::is_regular_file(report.backup_path / "holder.db"));

  holder::platform::Db recovered;
  recovered.open(paths.db_path());
  REQUIRE(holder::card::CardRepo(recovered).get(card.card_id).has_value());
  const auto restored_state =
      holder::api::support::load_thread_compaction_state(recovered, thread.thread_id);
  REQUIRE(restored_state.has_value());
  REQUIRE(restored_state->rolling_summary == compaction.rolling_summary);
  const auto restored_nudge = holder::ai::AiNudgeRepo(recovered).find_by_id("nudge-corrupt");
  REQUIRE(restored_nudge.has_value());
  REQUIRE(restored_nudge->dismissed);
  REQUIRE(holder::api::support::load_cloud_window_usage(
              recovered, "provider", "model", 0
          ).tokens == 12);
}

TEST_CASE("DatabaseRecovery identifies corrupt SQLite without replacing it", "[database][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  holder::core::Paths paths;
  paths.data_dir = dir / "data";
  paths.config_dir = dir / "config";
  paths.cache_dir = dir / "cache";
  paths.ensure_dirs();
  {
    std::ofstream out(paths.db_path(), std::ios::binary);
    out << "this is not sqlite";
  }
  const auto before_size = std::filesystem::file_size(paths.db_path());
  REQUIRE(holder::core::inspect_database_health(paths.db_path()).health ==
          holder::core::DatabaseHealth::Corrupt);

  auto secrets = holder::privacy::make_default_secret_store(paths.server_dir());
  REQUIRE_THROWS(holder::core::rebuild_database(paths, SCHEMA_SQL_PATH, *secrets, false));
  REQUIRE(std::filesystem::exists(paths.db_path()));
  REQUIRE(std::filesystem::file_size(paths.db_path()) == before_size);
}

TEST_CASE(
    "DatabaseRecovery preserves corrupt SQLite when a recovery authority is missing",
    "[database][recovery]"
) {
  const auto dir = holder::test::make_temp_dir();
  holder::core::Paths paths;
  paths.data_dir = dir / "data";
  paths.config_dir = dir / "config";
  paths.cache_dir = dir / "cache";
  paths.ensure_dirs();

  holder::platform::Db db;
  db.open(paths.db_path());
  holder::platform::Migrations::ensure_schema(db, SCHEMA_SQL_PATH);
  holder::core::ProjectRegistry(paths.project_registry_path()).remember({});
  holder::core::initialize_device_config(db, paths.device_config_path());
  holder::api::support::initialize_cloud_usage_ledger(db, paths.cloud_usage_ledger_path());
  holder::core::audit_durable_database_ownership(db, paths);
  holder::core::mark_database_rebuild_ready(paths);
  db.close();

  REQUIRE(std::filesystem::remove(paths.cloud_usage_ledger_path()));
  {
    std::ofstream damaged(paths.db_path(), std::ios::binary | std::ios::trunc);
    damaged << "not a sqlite database";
  }
  const auto before_size = std::filesystem::file_size(paths.db_path());
  auto secrets = holder::privacy::make_default_secret_store(paths.server_dir());
  REQUIRE_THROWS_WITH(
      holder::core::rebuild_database(paths, SCHEMA_SQL_PATH, *secrets, false),
      Catch::Matchers::ContainsSubstring("cloud usage ledger")
  );
  REQUIRE(std::filesystem::file_size(paths.db_path()) == before_size);
}

TEST_CASE("DatabaseRecovery blocks known SQLite-only durable state", "[database][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  holder::core::Paths paths;
  paths.data_dir = dir / "data";
  paths.config_dir = dir / "config";
  paths.cache_dir = dir / "cache";
  paths.ensure_dirs();
  holder::platform::Db db;
  db.open(paths.db_path());
  holder::platform::Migrations::ensure_schema(db, SCHEMA_SQL_PATH);
  db.exec(
      "INSERT INTO ai_local_model_config(key, fast_model, strong_model, deep_model, updated_at) "
      "VALUES('global', 'runner:model', NULL, NULL, 1);"
  );
  db.close();

  auto secrets = holder::privacy::make_default_secret_store(paths.server_dir());
  REQUIRE_THROWS_WITH(
      holder::core::rebuild_database(paths, SCHEMA_SQL_PATH, *secrets, false),
      Catch::Matchers::ContainsSubstring("local AI model configuration")
  );
}
