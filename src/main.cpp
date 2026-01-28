#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "core/LockFile.h"
#include "core/Paths.h"
#include "core/ServerInfo.h"
#include "core/Signal.h"
#include "api/HttpServer.h"
#include "store/CardStore.h"
#include "index/FtsIndexer.h"
#include "index/Reindexer.h"
#include "store/Db.h"
#include "store/Migrations.h"

#include <filesystem>
#include <chrono>
#include <memory>
#include <exception>
#include <string>

static std::filesystem::path find_schema_sql() {
  namespace fs = std::filesystem;

  // Dev-friendly: run from repo root
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;

  // Or if run from build/ directory
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;

  throw std::runtime_error("Cannot find schema/schema.sql from current directory.");
}

int main(int argc, char* argv[]) {
  auto paths = holder::core::Paths::resolve("holder");
  paths.ensure_dirs();

  const auto log_path = paths.log_dir() / "server.log";
  auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string(), true);
  auto logger = std::make_shared<spdlog::logger>("holder", spdlog::sinks_init_list{stdout_sink, file_sink});
  spdlog::set_default_logger(logger);
  const char* log_pattern = "[%Y-%m-%d %H:%M:%S.%e] [%l] %v";
  spdlog::set_pattern(log_pattern);

  auto console_logger = std::make_shared<spdlog::logger>("holder_console",
                                                         spdlog::sinks_init_list{stdout_sink});
  console_logger->set_pattern(log_pattern);
  spdlog::flush_on(spdlog::level::info);

  spdlog::info("holder starting…");
  spdlog::info("data_dir:   {}", paths.data_dir.string());
  spdlog::info("db_path:    {}", paths.db_path().string());
  spdlog::info("log_path:   {}", log_path.string());

  holder::core::SignalHandler signals;

  holder::core::LockFile lock(paths.lock_path());
  if (!lock.try_acquire()) {
    spdlog::error("Another holder instance appears to be running (lock busy: {}).",
                  paths.lock_path().string());
    return 2;
  }

  holder::store::Db db;
  db.open(paths.db_path());

  const auto schema_path = find_schema_sql();
  holder::store::Migrations::ensure_schema(db, schema_path);
  holder::store::Migrations::ensure_schema_version(db, 1);

  holder::core::ServerInfo info;
  info.started_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  info.api_version = "0.1";
  info.server_version = CARD_SERVER_VERSION;
  info.auth_token = holder::core::generate_auth_token();

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
      spdlog::info("Usage: holder [--bind <addr>] [--port <port>] [--reindex]");
      return 0;
    } else if (arg == "--reindex") {
      reindex_only = true;
    } else {
      spdlog::error("Unknown argument: {}", arg);
      return 2;
    }
  }

  spdlog::info("holder boot complete.");

  holder::index::FtsIndexer fts(db);
  if (reindex_only) {
    spdlog::info("Running full reindex...");
    holder::index::Reindexer reindexer(db);
    reindexer.run();
    spdlog::info("Reindex complete.");
    spdlog::shutdown();
    return 0;
  }
  holder::store::CardStore card_store(db, &fts);
  holder::api::HttpServer server(bind, port, db, info.auth_token, &card_store, &fts);
  const auto bound = server.start();

  info.pid = holder::core::current_pid();
  info.bind = bound.bind;
  info.port = bound.port;
  holder::core::write_server_info(paths.info_path(), info);
  spdlog::info("listening on {}:{}", info.bind, info.port);
  spdlog::info("docs available at http://{}:{}/docs", info.bind, info.port);
  console_logger->info("docs available at http://{}:{}/docs (auth token: {})",
                       info.bind,
                       info.port,
                       info.auth_token);

  server.run(signals);

  if (signals.is_requested()) {
    const int sig = signals.last_signal();
    const char* name = (sig == SIGINT) ? "SIGINT" : (sig == SIGTERM) ? "SIGTERM" : "unknown";
    spdlog::info("shutdown signal received: {}", name);
  }
  spdlog::info("holder shutdown complete.");
  spdlog::shutdown();
  return 0;
}
