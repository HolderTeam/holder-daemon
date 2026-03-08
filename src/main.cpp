#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "platform/LockFile.h"
#include "platform/Paths.h"
#include "platform/ServerInfo.h"
#include "platform/Signal.h"
#include "api/HttpServer.h"
#include "card/CardStore.h"
#include "project/ProjectRepo.h"
#include "model/Card.h"
#include "index/FtsIndexer.h"
#include "index/Reindexer.h"
#include "llm/LocalModelRunner.h"
#include "platform/Db.h"
#include "platform/Migrations.h"
#include "project/ProjectPaths.h"
#include "privacy/ProjectPrivacy.h"
#include "git/GitOps.h"
#include "sync/ProjectSyncWorker.h"

#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <optional>
#include <filesystem>
#include <chrono>
#include <memory>
#include <exception>
#include <string>
#include <fstream>
#include <sstream>

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

static std::string generate_uuid_v4() {
  boost::uuids::random_generator gen;
  return boost::uuids::to_string(gen());
}

static std::filesystem::path find_welcome_markdown() {
  namespace fs = std::filesystem;

  fs::path p1 = fs::current_path() / "config" / "WELCOME.md";
  if (fs::exists(p1)) return p1;

  fs::path p2 = fs::current_path().parent_path() / "config" / "WELCOME.md";
  if (fs::exists(p2)) return p2;

  throw std::runtime_error("Cannot find config/WELCOME.md from current directory.");
}

static std::string load_welcome_markdown_body() {
  std::ifstream in(find_welcome_markdown());
  if (!in) {
    throw std::runtime_error("Failed to open config/WELCOME.md");
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

static std::string derive_title_from_markdown_first_line(const std::string& body,
                                                         const std::string& fallback) {
  std::string line;
  for (char ch : body) {
    if (ch == '\n') break;
    line.push_back(ch);
  }
  const auto non_space = line.find_first_not_of(" \t\r");
  if (non_space == std::string::npos) {
    return fallback;
  }
  auto first = line.substr(non_space);
  if (!first.empty() && first[0] == '#') {
    const auto title_start = first.find_first_not_of("# \t");
    if (title_start != std::string::npos) {
      return first.substr(title_start);
    }
  }
  return fallback;
}

static std::optional<holder::model::Project> ensure_default_home_project(holder::store::Db& db) {
  holder::project::ProjectRepo repo(db);
  auto projects = repo.list();
  if (!projects.empty()) {
    return std::nullopt;
  }

  holder::model::Project home;
  home.project_id = generate_uuid_v4();
  home.name = "Home";
  home.privacy_mode = "encrypted_git";
  home.project_key_id.reset();
  home.created_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  home.updated_at = home.created_at;

  const auto base_root = holder::core::default_projects_root();
  const auto slug = holder::core::slugify(home.name);
  home.root_path = holder::core::unique_project_root(base_root, slug, projects);

  repo.create(home);
  spdlog::info("Bootstrapped default Home project ({})", home.project_id);
  return home;
}

static void ensure_default_welcome_card(holder::card::CardStore& card_store,
                                        const holder::model::Project& home) {
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const std::string content = load_welcome_markdown_body();
  holder::model::Card welcome;
  welcome.card_id = generate_uuid_v4();
  welcome.project_id = home.project_id;
  welcome.title = derive_title_from_markdown_first_line(content, "Welcome");
  welcome.created_at = now;
  welcome.updated_at = now;
  card_store.create(welcome, content);
  spdlog::info("Bootstrapped welcome card ({}) in Home project", welcome.card_id);
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
  const auto bootstrapped_home = ensure_default_home_project(db);

  holder::core::ServerInfo info;
  info.started_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  info.api_version = "0.1";
  info.server_version = CARD_SERVER_VERSION;
  info.auth_token = holder::core::generate_auth_token();

  spdlog::info("holder boot complete.");

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  if (bootstrapped_home.has_value()) {
    holder::project::ProjectRepo repo(db);
    holder::git::RealGitOps git;
    holder::privacy::ensure_encrypted_project_ready(
        git,
        repo,
        bootstrapped_home->project_id,
        bootstrapped_home->root_path,
        bootstrapped_home->project_key_id,
        bootstrapped_home->updated_at,
        generate_uuid_v4);
    ensure_default_welcome_card(card_store, bootstrapped_home.value());
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
  holder::api::HttpServer server(bind, port, db, info.auth_token, &card_store, &fts, nullptr, &runner);
  const auto bound = server.start();

  holder::sync::ProjectSyncWorker sync_worker(paths.db_path());
  std::thread sync_thread([&sync_worker, &signals]() {
    sync_worker.run(signals);
  });

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
  runner.stop();
  if (sync_thread.joinable()) {
    sync_thread.join();
  }

  if (signals.is_requested()) {
    const int sig = signals.last_signal();
    const char* name = (sig == SIGINT) ? "SIGINT" : (sig == SIGTERM) ? "SIGTERM" : "unknown";
    spdlog::info("shutdown signal received: {}", name);
  }
  spdlog::info("holder shutdown complete.");
  spdlog::shutdown();
  return 0;
}
