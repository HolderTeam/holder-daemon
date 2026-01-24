#include <spdlog/spdlog.h>

#include "core/LockFile.h"
#include "core/Paths.h"
#include "core/ServerInfo.h"
#include "store/Db.h"
#include "store/Migrations.h"
#include "git/GitRepo.h"

#include <filesystem>
#include <chrono>

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

int main() {
  spdlog::info("holder starting…");

  auto paths = holder::core::Paths::resolve("holder");
  paths.ensure_dirs();

  spdlog::info("data_dir:   {}", paths.data_dir.string());
  spdlog::info("db_path:    {}", paths.db_path().string());

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

  holder::core::ServerInfo info;
  info.pid = holder::core::current_pid();
  info.bind = "127.0.0.1";
  info.port = 0;
  info.started_at = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  info.api_version = "0.1";
  info.server_version = CARD_SERVER_VERSION;
  info.auth_token = holder::core::generate_auth_token();
  holder::core::write_server_info(paths.info_path(), info);

  holder::git::GitRepo repo;
  repo.open_or_init(paths.data_dir / "repo");

  // v0.1: write a placeholder export file
  repo.write_file("README.md",
    "# Holder\n\n"
    "This repository is managed by Holder.\n"
    "It contains exported cards and metadata for backup/sync.\n");

  repo.stage_path("README.md");
  repo.commit("Bootstrap holder repository");

  spdlog::info("holder boot complete.");
  return 0;
}
