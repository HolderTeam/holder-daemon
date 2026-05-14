#include "cli/commands/Commands.h"
#include "cli/commands/Common.h"
#include "platform/Paths.h"

#include <iostream>
#include <stdexcept>
#include <string>

#ifndef CARD_SERVER_VERSION
#define CARD_SERVER_VERSION "0.0.0"
#endif

int main(int argc, char* argv[]) {
  try {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
      holder::cli::print_usage(std::cout);
      return 0;
    }

    const std::string command = argv[1];
    if (command == "--version" || command == "version") {
      std::cout << "holderctl " << CARD_SERVER_VERSION << "\n";
      return 0;
    }
    if (command == "restart") return holder::cli::command_restart();

    const auto paths = holder::core::Paths::resolve("holder");
    if (command == "token") return holder::cli::command_token(paths);
    if (command == "status") return holder::cli::command_status(paths);
    if (command == "health") return holder::cli::command_health(paths);
    if (command == "paths") return holder::cli::command_paths(paths);
    if (command == "projects") return holder::cli::command_projects(paths, argc, argv);
    if (command == "use") return holder::cli::command_use(paths, argc, argv);
    if (command == "current") return holder::cli::command_current(paths, argc);
    if (command == "cards") return holder::cli::command_cards(paths, argc, argv);
    if (command == "search") return holder::cli::command_search(paths, argc, argv);
    if (command == "card") return holder::cli::command_card(paths, argc, argv);
    if (command == "edit") return holder::cli::command_edit(paths, argc, argv);
    if (command == "new") return holder::cli::command_new(paths, argc, argv);
    if (command == "append") return holder::cli::command_append(paths, argc, argv);
    if (command == "resource") return holder::cli::command_resource(paths, argc, argv);
    if (command == "recovery-token") return holder::cli::command_recovery_token(paths, argc, argv);
    if (command == "openapi") return holder::cli::command_openapi(paths, argc, argv);
    if (command == "logs") return holder::cli::command_logs(paths, argc, argv);
    if (command == "reindex") return holder::cli::command_reindex(paths, argc);

    std::cerr << "Unknown command: " << command << "\n";
    holder::cli::print_usage(std::cerr);
    return 2;
  } catch (const std::exception& ex) {
    std::cerr << "holderctl: " << ex.what() << "\n";
    return 1;
  }
}
