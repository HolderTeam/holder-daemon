#include "platform/DatabaseRecovery.h"

#include "ai/AiProviderCredentialRecovery.h"
#include "ai/AiNudgeDurability.h"
#include "ai/AiThreadStateDurability.h"
#include "ai/AiThreadManifest.h"
#include "ai/AiMessagePaths.h"
#include "api/support/CloudQuota.h"
#include "index/FtsIndexer.h"
#include "platform/Db.h"
#include "platform/DeviceConfigStore.h"
#include "platform/Migrations.h"
#include "platform/ProjectRegistry.h"
#include "project/ProjectManifest.h"
#include "project/StartupRecovery.h"
#include "resource/ResourcePaths.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace holder::core {
namespace {

std::string health_name(DatabaseHealth health) {
  switch (health) {
  case DatabaseHealth::Missing: return "missing";
  case DatabaseHealth::Healthy: return "healthy";
  case DatabaseHealth::Corrupt: return "corrupt";
  case DatabaseHealth::IoError: return "io_error";
  }
  return "unknown"; // LCOV_EXCL_LINE
}

bool looks_like_project(const std::filesystem::path& root) {
  return std::filesystem::is_directory(root) &&
         (std::filesystem::exists(root / ".holder" / "project.json") ||
          std::filesystem::exists(root / "cards") ||
          std::filesystem::exists(root / "ai_messages") ||
          std::filesystem::exists(root / "resources"));
}

std::vector<std::filesystem::path> discover_roots(const Paths& paths) {
  std::vector<std::filesystem::path> roots;
  const auto managed = std::getenv("HOLDER_PROJECTS_ROOT") != nullptr
                           ? std::filesystem::path(std::getenv("HOLDER_PROJECTS_ROOT"))
                           : paths.data_dir / "projects";
  if (std::filesystem::is_directory(managed)) {
    for (const auto& entry : std::filesystem::directory_iterator(managed)) {
      if (looks_like_project(entry.path())) roots.push_back(entry.path());
    }
  }
  const auto registered = ProjectRegistry(paths.project_registry_path()).roots();
  roots.insert(roots.end(), registered.begin(), registered.end());

  for (auto& root : roots) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(root, ec);
    if (!ec) root = canonical;
  }
  std::sort(roots.begin(), roots.end());
  roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
  return roots;
}

std::map<std::string, long long> durable_counts(holder::platform::Db& db) {
  static const std::vector<std::string> tables = {
      "projects", "cards", "card_links", "milestones", "resources", "resource_metadata",
      "storage_locations", "assets", "asset_placements", "ai_threads", "ai_messages",
  };
  std::map<std::string, long long> result;
  for (const auto& table : tables) {
    sqlite3_stmt* stmt = nullptr;
    const auto sql = "SELECT COUNT(*) FROM " + table + ";";
    if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      throw std::runtime_error("failed to count " + table + ": " + sqlite3_errmsg(db.handle()));
    }
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("failed to count " + table);
    }
    result[table] = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return result;
}

long long scalar_count(holder::platform::Db& db, const std::string& sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("database ownership audit failed: " + std::string(sqlite3_errmsg(db.handle())));
  }
  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    throw std::runtime_error("database ownership audit failed");
  }
  const auto value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

void require_no_sqlite_only_state(holder::platform::Db& db, const Paths& paths) {
  std::vector<std::pair<std::string, std::string>> checks;
  if (!holder::ai::all_thread_compaction_states_are_durable(db)) {
    checks.push_back({
        "AI thread compaction state", "SELECT COUNT(*) FROM ai_thread_compaction_state;"
    });
  }
  if (!holder::ai::all_nudge_dismissals_are_durable(db)) {
    checks.push_back({
        "dismissed AI nudges", "SELECT COUNT(*) FROM ai_nudges WHERE dismissed_at IS NOT NULL;"
    });
  }
  if (!std::filesystem::exists(paths.cloud_usage_ledger_path())) {
    checks.insert(checks.begin(), {
        {"cloud usage ledger", "SELECT COUNT(*) FROM ai_cloud_usage_events;"},
    });
  }
  if (!std::filesystem::exists(paths.device_config_path())) {
    checks.insert(checks.begin(), {
        {"local AI model configuration", "SELECT COUNT(*) FROM ai_local_model_config;"},
        {"manual AI runners", "SELECT COUNT(*) FROM ai_runners WHERE source <> 'auto_local';"},
        {"AI provider settings", "SELECT COUNT(*) FROM ai_provider_settings;"},
    });
  }
  std::vector<std::string> blockers;
  for (const auto& [label, sql] : checks) {
    if (scalar_count(db, sql) > 0) blockers.push_back(label);
  }
  if (!blockers.empty()) {
    std::string message = "database rebuild blocked because durable state still exists only in SQLite: ";
    for (std::size_t i = 0; i < blockers.size(); ++i) {
      if (i != 0) message += ", ";
      message += blockers[i];
    }
    throw std::runtime_error(message);
  }
}

void require_recovery_authorities(const Paths& paths) {
  const std::vector<std::pair<std::string, std::filesystem::path>> required = {
      {"project registry", paths.project_registry_path()},
      {"device configuration", paths.device_config_path()},
      {"cloud usage ledger", paths.cloud_usage_ledger_path()},
  };
  for (const auto& [label, path] : required) {
    if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error(
          "database recovery authority is missing its " + label + ": " + path.string()
      );
    }
  }
}

void validate_database(holder::platform::Db& db) {
  auto check = [&](const std::string& pragma, const std::string& expected) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db.handle(), pragma.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
      throw std::runtime_error("database validation prepare failed");
    }
    const int rc = sqlite3_step(stmt);
    const std::string value = rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)
                                  ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))
                                  : std::string();
    sqlite3_finalize(stmt);
    if (value != expected) throw std::runtime_error("database validation failed: " + pragma);
  };
  check("PRAGMA integrity_check;", "ok");

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), "PRAGMA foreign_key_check;", -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("foreign key validation prepare failed");
  }
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("foreign key validation failed");
}

std::filesystem::path unique_backup_dir(const Paths& paths, bool corrupt) {
  const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch()
  ).count();
  const auto base = paths.server_dir() / "database-backups";
  for (int suffix = 0; suffix < 1000; ++suffix) {
    const auto name = std::string(corrupt ? "quarantine-" : "backup-") +
                      std::to_string(timestamp) + (suffix == 0 ? "" : "-" + std::to_string(suffix));
    const auto candidate = base / name;
    if (!std::filesystem::exists(candidate)) return candidate;
  }
  throw std::runtime_error("unable to allocate database backup directory");
}

void move_if_exists(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (!std::filesystem::exists(from)) return;
  std::filesystem::rename(from, to);
}

void sync_directory(const std::filesystem::path& path) {
#ifndef _WIN32
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) throw std::runtime_error("failed to open database directory for fsync");
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) throw std::runtime_error("failed to fsync database directory");
#else
  (void)path;
#endif
}

void require_file_rows(
    holder::platform::Db& db,
    const std::string& label,
    const std::string& sql
) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare durable ownership audit failed for " + label);
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* root_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto* rel_text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!root_text || !rel_text ||
        !std::filesystem::is_regular_file(std::filesystem::path(root_text) / rel_text)) {
      sqlite3_finalize(stmt);
      throw std::runtime_error(label + " exists only in SQLite or has a missing durable file");
    }
  }
  sqlite3_finalize(stmt);
}

void write_readiness_file(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to write database rebuild readiness marker");
    out << nlohmann::json{{"version", 1}, {"durable_owner_generation", 1}}.dump(2) << '\n';
    out.flush();
    if (!out) throw std::runtime_error("failed to flush database rebuild readiness marker");
  }
#ifndef _WIN32
  if (::chmod(temporary.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("failed to restrict database rebuild readiness marker");
  }
#endif
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
#ifdef _WIN32
  if (ec && std::filesystem::exists(path)) {
    std::filesystem::remove(path, ec);
    if (!ec) std::filesystem::rename(temporary, path, ec);
  }
#endif
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("failed to replace database rebuild readiness marker: " + ec.message());
  }
}

} // namespace

std::string DatabaseRebuildReport::to_json() const {
  return nlohmann::json{
      {"ok", true},
      {"dry_run", dry_run},
      {"previous_health", previous_health},
      {"projects", projects},
      {"cards", cards},
      {"ai_threads", ai_threads},
      {"ai_messages", ai_messages},
      {"resources", resources},
      {"assets", assets},
      {"placements", placements},
      {"locations", locations},
      {"regenerated", {"card_tags", "full_text_search", "git_sync_status"}},
      {"reset", {"ai_run_history", "model_cooldowns", "transient_retry_state"}},
      {"warnings", nlohmann::json::array()},
      {"backup_path", backup_path.empty() ? nlohmann::json(nullptr)
                                            : nlohmann::json(backup_path.string())},
  }.dump(2);
}

DatabaseHealthResult inspect_database_health(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) return {DatabaseHealth::Missing, "database file is absent"};

  sqlite3* db = nullptr;
  const int open_rc = sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READONLY, nullptr);
  if (open_rc != SQLITE_OK) {
    const int code = db ? sqlite3_extended_errcode(db) : open_rc;
    const std::string message = db ? sqlite3_errmsg(db) : "sqlite open failed";
    if (db) sqlite3_close(db);
    const bool corrupt = code == SQLITE_CORRUPT || code == SQLITE_NOTADB;
    return {corrupt ? DatabaseHealth::Corrupt : DatabaseHealth::IoError, message};
  }

  sqlite3_stmt* stmt = nullptr;
  const int prepare_rc = sqlite3_prepare_v2(db, "PRAGMA quick_check;", -1, &stmt, nullptr);
  if (prepare_rc != SQLITE_OK) {
    const int code = sqlite3_extended_errcode(db);
    const std::string message = sqlite3_errmsg(db);
    sqlite3_close(db);
    const bool corrupt = code == SQLITE_CORRUPT || code == SQLITE_NOTADB;
    return {corrupt ? DatabaseHealth::Corrupt : DatabaseHealth::IoError, message};
  }
  const int step_rc = sqlite3_step(stmt);
  const std::string result = step_rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)
                                 ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))
                                 : std::string();
  const int code = sqlite3_extended_errcode(db);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (step_rc == SQLITE_ROW && result == "ok") return {DatabaseHealth::Healthy, "ok"};
  if (code == SQLITE_CORRUPT || code == SQLITE_NOTADB || step_rc == SQLITE_CORRUPT ||
      step_rc == SQLITE_NOTADB || !result.empty()) {
    return {DatabaseHealth::Corrupt, result.empty() ? "quick_check failed" : result};
  }
  return {DatabaseHealth::IoError, "quick_check could not complete"};
}

void audit_durable_database_ownership(holder::platform::Db& db, const Paths& paths) {
  require_no_sqlite_only_state(db, paths);
  if (!std::filesystem::is_regular_file(paths.project_registry_path())) {
    throw std::runtime_error("project registry has not been externalized");
  }
  if (!std::filesystem::is_regular_file(paths.device_config_path())) {
    throw std::runtime_error("device configuration has not been externalized");
  }
  if (!std::filesystem::is_regular_file(paths.cloud_usage_ledger_path())) {
    throw std::runtime_error("cloud usage ledger has not been externalized");
  }
  require_file_rows(
      db, "project metadata",
      "SELECT root_path, '.holder/privacy.json' FROM projects "
      "UNION ALL SELECT root_path, '.holder/project.json' FROM projects;"
  );
  require_file_rows(
      db, "card", "SELECT p.root_path, c.rel_path FROM cards c JOIN projects p USING(project_id);"
  );
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, m.message_id, m.deleted_at FROM ai_messages m "
          "JOIN ai_threads t ON t.thread_id=m.thread_id "
          "JOIN projects p ON p.project_id=t.project_id;",
          -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare AI message ownership audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    const bool deleted = sqlite3_column_type(stmt, 2) != SQLITE_NULL;
    const auto rel = deleted ? holder::core::ai_message_trash_rel_path(id)
                             : holder::core::ai_message_rel_path(id);
    if (!std::filesystem::is_regular_file(root / rel)) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("AI message exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, t.thread_id FROM ai_threads t JOIN projects p USING(project_id);",
          -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare AI thread manifest audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!std::filesystem::is_regular_file(root / holder::ai::ai_thread_manifest_rel_path(id))) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("AI thread exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, r.resource_id FROM resources r JOIN projects p USING(project_id);",
          -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare Resource manifest audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!std::filesystem::is_regular_file(root / holder::resource::resource_rel_path(id))) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("Resource exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT p.root_path, l.location_id FROM storage_locations l "
          "JOIN projects p USING(project_id);",
          -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare Location manifest audit failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::filesystem::path root =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!std::filesystem::is_regular_file(root / holder::resource::location_rel_path(id))) {
      sqlite3_finalize(stmt);
      throw std::runtime_error("storage Location exists only in SQLite");
    }
  }
  sqlite3_finalize(stmt);
}

bool database_rebuild_is_ready(const Paths& paths) {
  const auto path = paths.database_rebuild_readiness_path();
  if (!std::filesystem::is_regular_file(path)) return false;
  try {
    std::ifstream in(path, std::ios::binary);
    const auto body = nlohmann::json::parse(in);
    return body.value("version", 0) == 1 && body.value("durable_owner_generation", 0) == 1;
  } catch (const std::exception&) {
    return false;
  }
}

void mark_database_rebuild_ready(const Paths& paths) {
  write_readiness_file(paths.database_rebuild_readiness_path());
}

DatabaseRebuildReport rebuild_database(
    const Paths& paths,
    const std::filesystem::path& schema_path,
    holder::privacy::SecretStore& secret_store,
    bool dry_run
) {
  const auto health = inspect_database_health(paths.db_path());
  if (health.health == DatabaseHealth::IoError) {
    throw std::runtime_error("database I/O failure is not safe to rebuild: " + health.detail);
  }
  const auto roots = discover_roots(paths);
  if (health.health == DatabaseHealth::Corrupt && !database_rebuild_is_ready(paths)) {
    throw std::runtime_error(
        "corrupt database was preserved because this profile has no completed durable-ownership audit"
    );
  }
  if (health.health == DatabaseHealth::Missing && !roots.empty() &&
      !database_rebuild_is_ready(paths)) {
    throw std::runtime_error(
        "database is missing but project data predates the durable-ownership readiness marker"
    );
  }
  if (health.health == DatabaseHealth::Corrupt ||
      (health.health == DatabaseHealth::Missing && !roots.empty())) {
    require_recovery_authorities(paths);
  }
  std::set<std::string> project_ids;
  for (const auto& root : roots) {
    if (!std::filesystem::is_directory(root)) {
      throw std::runtime_error("registered project root is unavailable: " + root.string());
    }
    const auto project = holder::project::read_project_manifest(root);
    if (!project_ids.insert(project.project_id).second) {
      throw std::runtime_error("duplicate project_id in discovered roots: " + project.project_id);
    }
  }

  std::map<std::string, long long> old_counts;
  if (health.health == DatabaseHealth::Healthy) {
    holder::platform::Db old_db;
    old_db.open(paths.db_path());
    audit_durable_database_ownership(old_db, paths);
    old_counts = durable_counts(old_db);
    old_db.close();
  }

  DatabaseRebuildReport report;
  report.dry_run = dry_run;
  report.previous_health = health_name(health.health);
  report.projects = roots.size();

  auto temporary = paths.db_path();
  temporary += ".rebuild.tmp";
  if (std::filesystem::exists(temporary) || std::filesystem::exists(temporary.string() + "-wal") ||
      std::filesystem::exists(temporary.string() + "-shm")) {
    throw std::runtime_error("stale rebuild temporary database exists: " + temporary.string());
  }

  std::map<std::string, long long> new_counts;
  try {
    holder::platform::Db rebuilt;
    rebuilt.open(temporary);
    holder::platform::Migrations::ensure_schema(rebuilt, schema_path);
    holder::platform::Migrations::ensure_schema_version(
        rebuilt, holder::platform::Migrations::latest_schema_version
    );
    if (std::filesystem::exists(paths.device_config_path())) {
      holder::core::restore_device_config(rebuilt, paths.device_config_path());
    }
    if (std::filesystem::exists(paths.cloud_usage_ledger_path())) {
      holder::api::support::restore_cloud_usage_ledger(
          rebuilt, paths.cloud_usage_ledger_path()
      );
    }
    holder::index::FtsIndexer fts(rebuilt);
    holder::project::recover_project_roots(
        rebuilt, &fts, roots, [] { return std::string("unused-in-strict-recovery"); }, true
    );
    holder::ai::restore_thread_compaction_states(rebuilt);
    holder::ai::restore_nudge_dismissals(rebuilt);
    holder::ai::recover_ai_provider_credentials_from_secret_store(rebuilt, secret_store);
    validate_database(rebuilt);
    new_counts = durable_counts(rebuilt);
    if (!old_counts.empty() && old_counts != new_counts) {
      throw std::runtime_error("rebuilt database durable object counts do not match source database");
    }
    rebuilt.close();
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::remove(temporary.string() + "-wal", ignored);
    std::filesystem::remove(temporary.string() + "-shm", ignored);
    throw;
  }

  report.projects = static_cast<std::size_t>(new_counts["projects"]);
  report.cards = static_cast<std::size_t>(new_counts["cards"]);
  report.ai_threads = static_cast<std::size_t>(new_counts["ai_threads"]);
  report.ai_messages = static_cast<std::size_t>(new_counts["ai_messages"]);
  report.resources = static_cast<std::size_t>(new_counts["resources"]);
  report.assets = static_cast<std::size_t>(new_counts["assets"]);
  report.placements = static_cast<std::size_t>(new_counts["asset_placements"]);
  report.locations = static_cast<std::size_t>(new_counts["storage_locations"]);

  if (dry_run) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    std::filesystem::remove(temporary.string() + "-wal", ignored);
    std::filesystem::remove(temporary.string() + "-shm", ignored);
    return report;
  }

  std::filesystem::path backup_dir;
  if (health.health != DatabaseHealth::Missing) {
    backup_dir = unique_backup_dir(paths, health.health == DatabaseHealth::Corrupt);
    std::filesystem::create_directories(backup_dir);
    move_if_exists(paths.db_path(), backup_dir / "holder.db");
    move_if_exists(paths.db_path().string() + "-wal", backup_dir / "holder.db-wal");
    move_if_exists(paths.db_path().string() + "-shm", backup_dir / "holder.db-shm");
  }

  try {
    std::filesystem::rename(temporary, paths.db_path());
    sync_directory(paths.db_path().parent_path());
    const auto final_health = inspect_database_health(paths.db_path());
    if (final_health.health != DatabaseHealth::Healthy) {
      throw std::runtime_error("replacement database failed final health check: " + final_health.detail);
    }
  } catch (...) {
    std::error_code ignored;
    std::filesystem::remove(paths.db_path(), ignored);
    if (!backup_dir.empty() && health.health == DatabaseHealth::Healthy) {
      move_if_exists(backup_dir / "holder.db", paths.db_path());
      move_if_exists(backup_dir / "holder.db-wal", paths.db_path().string() + "-wal");
      move_if_exists(backup_dir / "holder.db-shm", paths.db_path().string() + "-shm");
    }
    throw;
  }

  report.backup_path = backup_dir;
  mark_database_rebuild_ready(paths);
  return report;
}

} // namespace holder::core
