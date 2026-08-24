#pragma once

#include "platform/Db.h"

#include <filesystem>

namespace holder::core {

// Makes non-secret daemon settings durable outside the disposable SQLite
// projection. initialize_device_config installs the process-wide path used by
// route handlers and either imports an existing file or creates the initial
// file from a pre-upgrade database.
void initialize_device_config(
    holder::platform::Db& db,
    const std::filesystem::path& path
);
void persist_device_config(holder::platform::Db& db);
void restore_device_config(
    holder::platform::Db& db,
    const std::filesystem::path& path
);

} // namespace holder::core
