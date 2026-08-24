#pragma once

#include "platform/DatabaseRebuild.h"
#include "platform/Paths.h"
#include "privacy/SecretStore.h"

#include <filesystem>
#include <string>

namespace holder::core {

using DatabaseHealth = holder::platform::DatabaseHealth;
using DatabaseHealthResult = holder::platform::DatabaseHealthResult;
using DatabaseRebuildReport = holder::platform::DatabaseRebuildReport;

inline DatabaseHealthResult inspect_database_health(const std::filesystem::path& path) {
  return holder::platform::inspect_database_health(path);
}

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
