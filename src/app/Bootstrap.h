#pragma once

#include "model/Project.h"

#include <optional>
#include <string>

namespace holder::index {
class FtsIndexer;
} // namespace holder::index

namespace holder::platform {
class Db;
} // namespace holder::platform

namespace holder::app {

std::string generate_uuid_v4();

// If no project exists yet, creates a default encrypted "Home" project with a
// welcome card loaded from config/WELCOME.md. Does nothing otherwise.
std::optional<holder::model::Project> bootstrap_default_home_project(
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts
);

} // namespace holder::app
