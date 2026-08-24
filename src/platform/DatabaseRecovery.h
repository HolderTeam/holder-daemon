#pragma once

#include "platform/Paths.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <filesystem>
#include <string>

namespace holder::core {

enum class DatabaseHealth { Missing, Healthy, Corrupt, IoError };

struct DatabaseHealthResult {
  DatabaseHealth health = DatabaseHealth::Missing;
  std::string detail;
};

struct DatabaseRebuildReport {
  bool dry_run = false;
  std::string previous_health;
  std::size_t projects = 0;
  std::size_t cards = 0;
  std::size_t ai_threads = 0;
  std::size_t ai_messages = 0;
  std::size_t resources = 0;
  std::size_t assets = 0;
  std::size_t placements = 0;
  std::size_t locations = 0;
  std::filesystem::path backup_path;
  std::string to_json() const;
};

DatabaseHealthResult inspect_database_health(const std::filesystem::path& path);

void audit_durable_database_ownership(
    holder::platform::Db& db,
    const holder::core::Paths& paths
);
bool database_rebuild_is_ready(const holder::core::Paths& paths);
void mark_database_rebuild_ready(const holder::core::Paths& paths);

DatabaseRebuildReport rebuild_database(
    const holder::core::Paths& paths,
    const std::filesystem::path& schema_path,
    holder::privacy::SecretStore& secret_store,
    bool dry_run
);

} // namespace holder::core
