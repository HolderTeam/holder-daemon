#include "platform/DatabaseRecovery.h"

#include "ai/AiNudgeDurability.h"
#include "ai/AiProviderCredentialRecovery.h"
#include "ai/AiThreadStateDurability.h"
#include "api/support/CloudQuota.h"
#include "platform/DeviceConfigStore.h"
#include "platform/Migrations.h"
#include "platform/ProjectRegistry.h"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace holder::core {
namespace {

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
  return roots;
}

long long scalar_count(holder::platform::Db& db, const std::string& sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(
        "database ownership audit failed: " + std::string(sqlite3_errmsg(db.handle()))
    );
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

void require_no_daemon_sqlite_only_state(holder::platform::Db& db, const Paths& paths) {
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
    std::string message =
        "database rebuild blocked because durable state still exists only in SQLite: ";
    for (std::size_t i = 0; i < blockers.size(); ++i) {
      if (i != 0) message += ", ";
      message += blockers[i];
    }
    throw std::runtime_error(message);
  }
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("failed to open database schema: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

void audit_durable_database_ownership(holder::platform::Db& db, const Paths& paths) {
  holder::platform::audit_core_durable_ownership(db);
  require_no_daemon_sqlite_only_state(db, paths);
  if (!std::filesystem::is_regular_file(paths.project_registry_path())) {
    throw std::runtime_error("project registry has not been externalized");
  }
  if (!std::filesystem::is_regular_file(paths.device_config_path())) {
    throw std::runtime_error("device configuration has not been externalized");
  }
  if (!std::filesystem::is_regular_file(paths.cloud_usage_ledger_path())) {
    throw std::runtime_error("cloud usage ledger has not been externalized");
  }
}

bool database_rebuild_is_ready(const Paths& paths) {
  return holder::platform::database_rebuild_is_ready(
      paths.database_rebuild_readiness_path()
  );
}

void mark_database_rebuild_ready(const Paths& paths) {
  holder::platform::mark_database_rebuild_ready(paths.database_rebuild_readiness_path());
}

DatabaseRebuildReport rebuild_database(
    const Paths& paths,
    const std::filesystem::path& schema_path,
    holder::privacy::SecretStore& secret_store,
    bool dry_run
) {
  holder::platform::DatabaseRebuildRequest request;
  request.database_path = paths.db_path();
  request.backup_root = paths.server_dir() / "database-backups";
  request.schema_sql = read_text_file(schema_path);
  request.expected_schema_version = holder::platform::Migrations::latest_schema_version;
  request.project_roots = discover_roots(paths);
  request.required_authorities = {
      {"project registry", paths.project_registry_path()},
      {"device configuration", paths.device_config_path()},
      {"cloud usage ledger", paths.cloud_usage_ledger_path()},
  };
  request.durable_ownership_ready = database_rebuild_is_ready(paths);
  request.dry_run = dry_run;
  request.hooks.audit_existing = [&](holder::platform::Db& db) {
    require_no_daemon_sqlite_only_state(db, paths);
  };
  request.hooks.restore_before_projects = [&](holder::platform::Db& db) {
    if (std::filesystem::exists(paths.device_config_path())) {
      holder::core::restore_device_config(db, paths.device_config_path());
    }
    if (std::filesystem::exists(paths.cloud_usage_ledger_path())) {
      holder::api::support::restore_cloud_usage_ledger(db, paths.cloud_usage_ledger_path());
    }
  };
  request.hooks.restore_after_projects = [&](holder::platform::Db& db) {
    holder::ai::restore_thread_compaction_states(db);
    holder::ai::restore_nudge_dismissals(db);
    holder::ai::recover_ai_provider_credentials_from_secret_store(db, secret_store);
  };

  auto report = holder::platform::rebuild_database_projection(request);
  if (!dry_run) mark_database_rebuild_ready(paths);
  return report;
}

} // namespace holder::core
