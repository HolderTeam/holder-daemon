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
int command_project(const holder::core::Paths& paths, int argc, char* argv[]);
int command_projects(const holder::core::Paths& paths, int argc, char* argv[]);
int command_use(const holder::core::Paths& paths, int argc, char* argv[]);
int command_current(const holder::core::Paths& paths, int argc);
int command_cards(const holder::core::Paths& paths, int argc, char* argv[]);
int command_search(const holder::core::Paths& paths, int argc, char* argv[]);
int command_card(const holder::core::Paths& paths, int argc, char* argv[]);
int command_edit(const holder::core::Paths& paths, int argc, char* argv[]);
int command_links(const holder::core::Paths& paths, int argc, char* argv[]);
int command_backlinks(const holder::core::Paths& paths, int argc, char* argv[]);
int command_link(const holder::core::Paths& paths, int argc, char* argv[]);
int command_trash(const holder::core::Paths& paths, int argc, char* argv[]);
int command_restore(const holder::core::Paths& paths, int argc, char* argv[]);
int command_new(const holder::core::Paths& paths, int argc, char* argv[]);
int command_append(const holder::core::Paths& paths, int argc, char* argv[]);
int command_resource(const holder::core::Paths& paths, int argc, char* argv[]);
int command_recovery_token(const holder::core::Paths& paths, int argc, char* argv[]);

} // namespace holder::cli
