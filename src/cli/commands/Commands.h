#pragma once

#include "platform/Paths.h"

namespace holder::cli {

int command_token(const holder::core::Paths& paths);
int command_status(const holder::core::Paths& paths);
int command_health(const holder::core::Paths& paths);
int command_paths(const holder::core::Paths& paths);
int command_openapi(const holder::core::Paths& paths, int argc, char* argv[]);
int command_restart();
int command_logs(const holder::core::Paths& paths, int argc, char* argv[]);
int command_reindex(const holder::core::Paths& paths, int argc);
int command_projects(const holder::core::Paths& paths, int argc, char* argv[]);
int command_use(const holder::core::Paths& paths, int argc, char* argv[]);
int command_current(const holder::core::Paths& paths, int argc);

} // namespace holder::cli
