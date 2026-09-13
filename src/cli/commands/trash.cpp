#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace holder::cli {
namespace {

struct TrashOptions {
  bool json_output = false;
  std::string subcommand;
  std::string card_id;
};

TrashOptions parse_trash_options(int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(
        "Usage: holderctl trash <card-reference>|list|restore <card-reference>|delete <card-reference>|empty [--json]"
    ); // LCOV_EXCL_LINE
  }

  TrashOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown trash option: " + arg);
    } else if (options.subcommand.empty()) {
      if (arg == "list" || arg == "restore" || arg == "delete" || arg == "empty") {
        options.subcommand = arg;
      } else {
        options.card_id = arg;
        options.subcommand = "card";
      }
    } else if ((options.subcommand == "restore" || options.subcommand == "delete") &&
               options.card_id.empty()) {
      options.card_id = arg;
    } else {
      throw std::runtime_error(
          "Usage: holderctl trash <card-reference>|list|restore <card-reference>|delete <card-reference>|empty [--json]"
      );
    }
  }

  if (options.subcommand.empty()) {
    throw std::runtime_error(
        "Usage: holderctl trash <card-reference>|list|restore <card-reference>|delete <card-reference>|empty [--json]"
    ); // LCOV_EXCL_LINE
  }
  const bool needs_card = options.subcommand == "card" || options.subcommand == "restore" ||
                          options.subcommand == "delete";
  if (needs_card && options.card_id.empty()) {
    throw std::runtime_error(
        "Usage: holderctl trash <card-reference>|list|restore <card-reference>|delete <card-reference>|empty [--json]"
    );
  }
  if ((options.subcommand == "list" || options.subcommand == "empty") &&
      !options.card_id.empty(
      )) { // LCOV_EXCL_LINE: parser rejects extra args before this defensive check.
    throw std::runtime_error(
        "Usage: holderctl trash <card-reference>|list|restore <card-reference>|delete <card-reference>|empty [--json]"
    ); // LCOV_EXCL_LINE
  }
  return options;
}

TrashOptions parse_restore_options(int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error("Usage: holderctl restore <card-reference> [--json]");
  }

  TrashOptions options;
  options.subcommand = "restore";
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown restore option: " + arg);
    } else if (options.card_id.empty()) {
      options.card_id = arg;
    } else {
      throw std::runtime_error("Usage: holderctl restore <card-reference> [--json]");
    }
  }
  if (options.card_id.empty()) {
    throw std::runtime_error("Usage: holderctl restore <card-reference> [--json]");
  }
  return options;
}

nlohmann::json list_current_project_trash_payload(
    const holder::core::Paths& paths,
    const std::string& project_id
) {
  return card_api_request(
      paths,
      boost::beast::http::verb::get,
      "/trash?project_id=" + url_encode_component(project_id) + "&type=card"
  );
}

void print_trash_table(const nlohmann::json& trash) {
  if (!trash.is_array() || trash.empty()) {
    std::cout << "Trash is empty.\n";
    return;
  }

  std::cout << "CARD_ID\tTITLE\tDELETED\n";
  std::vector<std::string> card_ids;
  card_ids.reserve(trash.size());
  for (const auto& item : trash) {
    card_ids.push_back(json_string(item, "card_id"));
  }
  const auto displayed_ids = display_card_ids(card_ids);
  for (std::size_t i = 0; i < trash.size(); ++i) {
    const auto& item = trash.at(i);
    std::cout << displayed_ids.at(i) << "\t" << json_string(item, "title") << "\t"
              << item.value("deleted_at", 0) << "\n";
  }
}

int restore_trashed_card(
    const holder::core::Paths& paths,
    const std::string& current_project_id,
    const TrashOptions& options
) {
  const auto card_id = resolve_card_reference(
      paths,
      current_project_id,
      options.card_id,
      CardReferenceScope::Trashed
  );
  const auto payload = card_api_request(
      paths,
      boost::beast::http::verb::post,
      "/cards/" + url_encode_component(card_id) + "/restore"
  );
  if (options.json_output) {
    std::cout << payload.dump(2) << "\n";
  } else {
    std::cout << "Restored card: " << display_card_id(card_id) << "\n";
  }
  return 0;
}

} // namespace

int command_trash(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_trash_options(argc, argv);

  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");

    if (options.subcommand == "list") {
      const auto payload = list_current_project_trash_payload(paths, current_project_id);
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        print_trash_table(payload.at("data"));
      }
      return 0;
    }

    if (options.subcommand == "restore") {
      return restore_trashed_card(paths, current_project_id, options);
    }

    if (options.subcommand == "delete") {
      const auto card_id = resolve_card_reference(
          paths,
          current_project_id,
          options.card_id,
          CardReferenceScope::Trashed
      );
      const auto payload = card_api_request(
          paths,
          boost::beast::http::verb::delete_,
          "/trash/card/" + url_encode_component(card_id)
      );
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        std::cout << "Deleted trashed card: " << display_card_id(card_id) << "\n";
      }
      return 0;
    }

    if (options.subcommand == "empty") {
      const auto payload = card_api_request(
          paths,
          boost::beast::http::verb::delete_,
          "/trash?project_id=" + url_encode_component(current_project_id) + "&type=card"
      );
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        std::cout << "Emptied card trash.\n";
      }
      return 0;
    }

    const auto card_id = resolve_card_reference(
        paths,
        current_project_id,
        options.card_id,
        CardReferenceScope::Live
    );
    const auto payload = card_api_request(
        paths,
        boost::beast::http::verb::delete_,
        "/cards/" + url_encode_component(card_id)
    );
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      std::cout << "Trashed card: " << display_card_id(card_id) << "\n";
    }
    return 0;
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to manage trash: ") + ex.what());
  }
}

int command_restore(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_restore_options(argc, argv);

  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    return restore_trashed_card(paths, current_project_id, options);
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to restore card: ") + ex.what());
  }
}

} // namespace holder::cli
