#include "app/DaemonApp.h"

#include "ai/AiProviderCredentialRecovery.h"
#include "api/HttpServer.h"
#include "app/Bootstrap.h"
#include "app/RuntimeGuards.h"
#include "card/CardStore.h"
#include "core/ConcurrencyProfilePolicy.h"
#include "index/FtsIndexer.h"
#include "index/Reindexer.h"
#include "llm/LocalModelRunner.h"
#include "llm/LocalRunnerClient.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"
#include "platform/InstalledDataPath.h"
#include "platform/LockFile.h"
#include "platform/Migrations.h"
#include "platform/Paths.h"
#include "platform/ServerInfo.h"
#include "platform/Signal.h"
#include "privacy/SecretStore.h"
#include "project/ProjectRepo.h"
#include "project/StartupRecovery.h"
#include "sync/ProjectSyncWorker.h"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
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

namespace holder::app {
namespace {

void print_usage(std::ostream& out) {
  out << "Usage: holderd [--help] [--version] [--bind <addr>] [--port <port>] [--reindex]\n";
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

  holder::platform::Db db;
  db.open(paths.db_path());

  const auto schema_path = find_schema_sql();
  holder::platform::Migrations::ensure_schema(db, schema_path);
  holder::platform::Migrations::ensure_schema_version(db, 1);
  holder::index::FtsIndexer fts(db);
  auto secret_store = holder::privacy::make_default_secret_store(paths.server_dir());

  holder::project::ProjectRepo project_repo(db);
  if (project_repo.list().empty()) {
    holder::project::recover_projects_from_disk(
        db,
        &fts,
        holder::core::default_projects_root(),
        generate_uuid_v4
    );
  }
  holder::ai::recover_ai_provider_credentials_from_secret_store(db, *secret_store);
  holder::app::bootstrap_default_home_project(db, &fts);

  holder::core::ServerInfo info;
  info.started_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
  )
                        .count();
  info.api_version = "0.1";
  info.server_version = CARD_SERVER_VERSION;
  info.auth_token = holder::core::generate_auth_token();

  spdlog::info("holder boot complete.");

  holder::card::CardStore card_store(db, &fts);
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

  server.run(signals);
  const bool shutdown_signal_received = signals.is_requested();
  runner.stop();
  sync_thread.stop_and_join();

  if (shutdown_signal_received) {
    spdlog::info("shutdown signal received: {}", holder::core::signal_name(signals.last_signal()));
  }
  spdlog::info("holder shutdown complete."); // LCOV_EXCL_LINE
  spdlog::shutdown(); // LCOV_EXCL_LINE
  return 0; // LCOV_EXCL_LINE
}

} // namespace holder::app
