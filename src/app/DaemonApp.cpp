#include "app/DaemonApp.h"

#include "ai/AiProviderCredentialRecovery.h"
#include "ai/AiNudgeDurability.h"
#include "ai/AiThreadDurability.h"
#include "ai/AiThreadStateDurability.h"
#include "api/HttpServer.h"
#include "api/support/CloudQuota.h"
#include "app/Bootstrap.h"
#include "app/RuntimeGuards.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "card/TagExtractor.h"
#include "card/TagRepo.h"
#include "core/ConcurrencyProfilePolicy.h"
#include "index/FtsIndexer.h"
#include "index/Reindexer.h"
#include "git/GitOps.h"
#include "llm/LocalModelRunner.h"
#include "llm/LocalRunnerClient.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"
#include "platform/DatabaseRecovery.h"
#include "platform/DeviceConfigStore.h"
#include "platform/InstalledDataPath.h"
#include "platform/LockFile.h"
#include "platform/Migrations.h"
#include "platform/Paths.h"
#include "platform/ProjectRegistry.h"
#include "platform/ServerInfo.h"
#include "platform/Signal.h"
#include "privacy/SecretStore.h"
#include "project/ProjectRepo.h"
#include "project/ProjectManifest.h"
#include "project/StartupRecovery.h"
#include "sync/ProjectSyncWorker.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <atomic>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#ifndef CARD_SERVER_VERSION
#define CARD_SERVER_VERSION "0.0.0"
#endif
#ifndef CARD_SERVER_API_VERSION
#define CARD_SERVER_API_VERSION "0.0"
#endif

namespace holder::app {
namespace {

void print_usage(std::ostream& out) {
  out << "Usage: holderd [--help] [--version] [--bind <addr>] [--port <port>] [--reindex] "
         "[--rebuild-database [--dry-run]]\n";
}

std::filesystem::path find_schema_sql() {
  namespace fs = std::filesystem;

  // Dev-friendly: run from repo root
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;

  // Or if run from build/ directory
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;

  if (auto installed = holder::core::installed_data_path("schema/schema.sql")) // LCOV_EXCL_LINE
    return installed.value(); // LCOV_EXCL_LINE

  throw std::runtime_error("Cannot find schema/schema.sql from current directory."
  ); // LCOV_EXCL_LINE
}

void backfill_card_tags(holder::platform::Db& db, holder::card::CardStore& card_store) {
  holder::project::ProjectRepo project_repo(db);
  holder::card::CardRepo card_repo(db);
  holder::card::TagRepo tag_repo(db);
  std::size_t indexed_cards = 0;
  std::size_t indexed_tags = 0;

  for (const auto& project : project_repo.list()) {
    for (const auto& card : card_repo.list_all(project.project_id)) {
      if (card.deleted_at.has_value()) continue;

      try {
        const auto content = card_store.get_content(card);
        if (!content.has_value()) {
          spdlog::warn("Skipping tag backfill for card with missing content: {}", card.card_id);
          continue;
        }
        const auto tags = holder::core::extract_tags(content.value());
        tag_repo.set_tags_for_card(project.project_id, card.card_id, tags, card.updated_at);
        ++indexed_cards;
        indexed_tags += tags.size();
      } catch (const std::exception& ex) {
        spdlog::warn("Skipping tag backfill for card {}: {}", card.card_id, ex.what());
      }
    }
  }

  spdlog::info("Tag index backfill complete: {} cards, {} tags.", indexed_cards, indexed_tags);
}

void backfill_project_manifests(holder::platform::Db& db) {
  holder::project::ProjectRepo projects(db);
  for (const auto& project : projects.list()) {
    if (holder::project::has_project_manifest(project.root_path)) continue;

    holder::git::RealGitOps git;
    holder::project::write_project_manifest(git, project);
    git.commit("Add durable project metadata");
    spdlog::info("Added durable project metadata: {}", project.root_path);
  }
}

} // namespace

int run_daemon(int argc, char* argv[]) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--version") {
      std::cout << "holderd " << CARD_SERVER_VERSION << "\n";
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      print_usage(std::cout);
      return 0;
    }
  }

  auto paths = holder::core::Paths::resolve("holder");
  paths.ensure_dirs();

  const auto log_path = paths.log_dir() / "server.log";
  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string(), true);
  auto logger =
      std::make_shared<spdlog::logger>("holder", spdlog::sinks_init_list{stdout_sink, file_sink});
  spdlog::set_default_logger(logger);
  const char* log_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";
  spdlog::set_pattern(log_pattern);

  auto console_logger =
      std::make_shared<spdlog::logger>("holder_console", spdlog::sinks_init_list{stdout_sink});
  console_logger->set_pattern(log_pattern);
  spdlog::flush_on(spdlog::level::info);

  spdlog::info("holder starting…");
  spdlog::info("data_dir:   {}", paths.data_dir.string());
  spdlog::info("db_path:    {}", paths.db_path().string());
  spdlog::info("log_path:   {}", log_path.string());

  std::string bind = "127.0.0.1";
  unsigned short port = 11499;
  bool reindex_only = false;
  bool rebuild_database_only = false;
  bool rebuild_dry_run = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--bind" && i + 1 < argc) {
      bind = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      try {
        const int parsed = std::stoi(argv[++i]);
        if (parsed < 0 || parsed > 65535) {
          spdlog::error("Invalid --port value: {}", parsed);
          return 2;
        }
        port = static_cast<unsigned short>(parsed);
      } catch (const std::exception& ex) {
        spdlog::error("Invalid --port value: {} ({})", argv[i], ex.what());
        return 2;
      }
    } else if (arg == "--help" || arg == "-h") {
      print_usage(std::cout); // LCOV_EXCL_LINE: handled by the pre-logging argument pass.
      return 0; // LCOV_EXCL_LINE
    } else if (arg == "--reindex") {
      reindex_only = true;
    } else if (arg == "--rebuild-database") {
      rebuild_database_only = true;
    } else if (arg == "--dry-run") {
      rebuild_dry_run = true;
    } else {
      spdlog::error("Unknown argument: {}", arg);
      return 2;
    }
  }

  holder::core::SignalHandler signals;

  holder::core::LockFile lock(paths.lock_path());
  if (!lock.try_acquire()) {
    spdlog::error(
        "Another holder instance appears to be running (lock busy: {}).",
        paths.lock_path().string()
    );
    return 2;
  }

  const auto schema_path = find_schema_sql();
  auto secret_store = holder::privacy::make_default_secret_store(paths.server_dir());
  if (rebuild_database_only) {
    const auto report = holder::core::rebuild_database(
        paths, schema_path, *secret_store, rebuild_dry_run
    );
    std::cout << report.to_json() << '\n';
    spdlog::shutdown();
    return 0;
  }
  if (rebuild_dry_run) {
    spdlog::error("--dry-run requires --rebuild-database");
    return 2;
  }

  const auto startup_health = holder::core::inspect_database_health(paths.db_path());
  if (startup_health.health == holder::core::DatabaseHealth::IoError) {
    throw std::runtime_error("database I/O failure: " + startup_health.detail);
  }
  if (startup_health.health == holder::core::DatabaseHealth::Missing ||
      startup_health.health == holder::core::DatabaseHealth::Corrupt) {
    spdlog::warn(
        "Database health is {}; rebuilding the local projection before startup.",
        startup_health.health == holder::core::DatabaseHealth::Missing ? "missing" : "corrupt"
    );
    const auto report = holder::core::rebuild_database(paths, schema_path, *secret_store, false);
    spdlog::info("Database reconstruction completed: {}", report.to_json());
  }

  const bool database_existed = std::filesystem::exists(paths.db_path());
  holder::platform::Db db;
  db.open(paths.db_path());

  holder::platform::Migrations::ensure_schema(db, schema_path);
  const bool schema_migrated = holder::platform::Migrations::migrate_to_latest(db);
  holder::platform::Migrations::ensure_schema_version(
      db, holder::platform::Migrations::latest_schema_version
  );
  holder::core::initialize_device_config(db, paths.device_config_path());
  holder::api::support::initialize_cloud_usage_ledger(db, paths.cloud_usage_ledger_path());
  holder::index::FtsIndexer fts(db);

  holder::project::ProjectRepo project_repo(db);
  if (project_repo.list().empty()) {
    holder::project::recover_projects_from_disk(
        db,
        &fts,
        holder::core::default_projects_root(),
        generate_uuid_v4,
        !database_existed
    );
    holder::core::ProjectRegistry registry(paths.project_registry_path());
    const auto registered_roots = registry.roots();
    if (!registered_roots.empty()) {
      holder::project::recover_project_roots(
          db,
          &fts,
          registered_roots,
          generate_uuid_v4,
          !database_existed
      );
    }
  }
  holder::ai::recover_ai_provider_credentials_from_secret_store(db, *secret_store);
  holder::app::bootstrap_default_home_project(db, &fts);
  backfill_project_manifests(db);
  const auto thread_manifests_added = holder::ai::backfill_ai_thread_manifests(db);
  if (thread_manifests_added > 0) {
    spdlog::info("Added durable metadata for {} AI threads.", thread_manifests_added);
  }
  const auto thread_states_added = holder::ai::backfill_thread_compaction_states(db);
  if (thread_states_added > 0) {
    spdlog::info("Added durable state for {} AI threads.", thread_states_added);
  }
  const auto nudge_dismissals_added = holder::ai::backfill_nudge_dismissals(db);
  if (nudge_dismissals_added > 0) {
    spdlog::info("Added durable tombstones for {} dismissed AI nudges.", nudge_dismissals_added);
  }
  holder::core::ProjectRegistry(paths.project_registry_path()).remember(project_repo.list());
  try {
    holder::core::audit_durable_database_ownership(db, paths);
    holder::core::mark_database_rebuild_ready(paths);
  } catch (const std::exception& ex) {
    spdlog::warn("Database rebuild readiness audit is incomplete: {}", ex.what());
  }

  holder::core::ServerInfo info;
  info.started_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
  )
                        .count();
  info.api_version = CARD_SERVER_API_VERSION;
  info.server_version = CARD_SERVER_VERSION;
  info.auth_token = holder::core::generate_auth_token();

  spdlog::info("holder boot complete.");

  holder::card::CardStore card_store(db, &fts);
  if (schema_migrated) {
    spdlog::info("Rebuilding the tag index after schema migration...");
    backfill_card_tags(db, card_store);
  }
  if (reindex_only) {
    spdlog::info("Running full reindex...");
    holder::index::Reindexer reindexer(db);
    reindexer.run();
    spdlog::info("Reindex complete.");
    spdlog::shutdown();
    return 0;
  }
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  local_runner_client.start_background_probe();
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const CasteResult machine_caste = detect_caste();
  const auto concurrency = holder::core::concurrency_profile_for_caste(machine_caste.caste);
  spdlog::info("machine caste: {} ({})", caste_name(machine_caste.caste), machine_caste.reason);
  spdlog::info(
      "listener concurrency: io={}, ingress={}, save={}, general={}, writer={}",
      concurrency.io_threads,
      concurrency.ingress_workers,
      concurrency.save_workers,
      concurrency.general_workers,
      concurrency.writer_workers
  );
  holder::api::HttpServer server(
      bind,
      port,
      db,
      info.auth_token,
      &card_store,
      &fts,
      nullptr,
      &runner_registry,
      concurrency
  );
  const auto bound = server.start();

  holder::sync::ProjectSyncWorker sync_worker(paths.db_path());
  std::thread sync_worker_thread([&sync_worker, &signals]() { // LCOV_EXCL_LINE
    sync_worker.run(signals);
  });
  SyncThreadGuard sync_thread(signals, std::move(sync_worker_thread));

  info.pid = holder::core::current_pid();
  info.bind = bound.bind;
  info.port = bound.port;
  holder::core::write_server_info(paths.info_path(), info);
  spdlog::info("listening on {}:{}", info.bind, info.port);
  spdlog::info("docs available at http://{}:{}/docs", info.bind, info.port);
  console_logger->info(
      "docs available at http://{}:{}/docs (auth token: {})",
      info.bind,
      info.port,
      info.auth_token
  );

  std::atomic<bool> database_health_failure{false};
  std::atomic<bool> database_health_stop_requested{false};
  std::thread database_health_monitor_thread(
      [&]() { // LCOV_EXCL_LINE: requires live filesystem corruption.
        while (!database_health_stop_requested.load() && !signals.is_requested()) {
          for (int tenth_seconds = 0;
               tenth_seconds < 50 && !database_health_stop_requested.load() &&
               !signals.is_requested();
               ++tenth_seconds) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
          }
          if (database_health_stop_requested.load() || signals.is_requested()) break;
          const auto health = holder::core::inspect_database_health(paths.db_path());
          if (health.health == holder::core::DatabaseHealth::Healthy) continue;
          database_health_failure.store(true);
          if (health.health == holder::core::DatabaseHealth::Corrupt) {
            spdlog::critical(
                "SQLite corruption detected during runtime; stopping all work so the database can "
                "be quarantined and rebuilt on the next start: {}",
                health.detail
            );
          } else {
            spdlog::critical(
                "SQLite health check failed during runtime; stopping without classifying the "
                "failure as corruption: {}",
                health.detail
            );
          }
          signals.request_stop();
          server.stop();
          break;
        }
      }
  );
  StopFlagThreadGuard database_health_monitor(
      database_health_stop_requested,
      std::move(database_health_monitor_thread)
  );

  server.run(signals);
  database_health_monitor.stop_and_join();
  const bool shutdown_signal_received = signals.is_requested();
  runner.stop();
  sync_thread.stop_and_join();

  if (shutdown_signal_received) {
    spdlog::info("shutdown signal received: {}", holder::core::signal_name(signals.last_signal()));
  }
  spdlog::info("holder shutdown complete."); // LCOV_EXCL_LINE
  spdlog::shutdown(); // LCOV_EXCL_LINE
  return database_health_failure.load() ? 3 : 0; // LCOV_EXCL_LINE
}

} // namespace holder::app
