#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace holder::cli {
namespace {

struct LinkListOptions {
  bool json_output = false;
  bool include_deleted = false;
  std::string card_id;
};

struct LinkCreateOptions {
  bool json_output = false;
  std::string from_card_id;
  std::string to_card_id;
  std::string kind = "ref";
  std::optional<std::string> label;
};

LinkListOptions parse_link_list_options(
    int argc,
    char* argv[],
    const std::string& usage,
    const std::string& command_name
) {
  LinkListOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--include-deleted") {
      options.include_deleted = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown " + command_name + " option: " + arg);
    } else if (options.card_id.empty()) {
      options.card_id = arg;
    } else {
      throw std::runtime_error(usage);
    }
  }

  if (options.card_id.empty()) {
    throw std::runtime_error(usage);
  }
  return options;
}

LinkCreateOptions parse_link_create_options(int argc, char* argv[]) {
  LinkCreateOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& usage) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(usage);
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error("link option value must not be empty");
      }
      return value;
    };

    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--kind") {
      options.kind = require_value(
          "Usage: holderctl link <from-card-id> <to-card-id> [--kind <kind>] [--label <label>] [--json]"
      );
    } else if (arg == "--label") {
      options.label = require_value(
          "Usage: holderctl link <from-card-id> <to-card-id> [--kind <kind>] [--label <label>] [--json]"
      );
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown link option: " + arg);
    } else if (options.from_card_id.empty()) {
      options.from_card_id = arg;
    } else if (options.to_card_id.empty()) {
      options.to_card_id = arg;
    } else {
      throw std::runtime_error(
          "Usage: holderctl link <from-card-id> <to-card-id> [--kind <kind>] [--label <label>] [--json]"
      );
    }
  }

  if (options.from_card_id.empty() || options.to_card_id.empty()) {
    throw std::runtime_error(
        "Usage: holderctl link <from-card-id> <to-card-id> [--kind <kind>] [--label <label>] [--json]"
    );
  }
  return options;
}

void print_links_table(const nlohmann::json& links, bool backlinks) {
  if (!links.is_array() || links.empty()) {
    std::cout << (backlinks ? "No backlinks.\n" : "No links.\n");
    return;
  }

  std::cout << (backlinks ? "FROM_ID" : "TO_ID") << "\tTYPE\tKIND\tLABEL\tCREATED\n";
  for (const auto& link : links) {
    std::cout << json_string(link, backlinks ? "from_card_id" : "to_card_id") << "\t"
              << json_string(link, "to_type") << "\t" << json_string(link, "kind") << "\t"
              << json_string(link, "label") << "\t" << link.value("created_at", 0) << "\n";
  }
}

} // namespace

int command_links(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_link_list_options(
      argc,
      argv,
      "Usage: holderctl links <card-id> [--include-deleted] [--json]",
      "links"
  );

  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    (void)fetch_card_in_current_project(paths, current_project_id, options.card_id);

    std::string target = "/cards/" + url_encode_component(options.card_id) + "/links";
    if (options.include_deleted) {
      target += "?include_deleted=1";
    }
    const auto payload = card_api_request(paths, boost::beast::http::verb::get, target);
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      print_links_table(payload.at("data"), false);
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list links: ") + ex.what());
  }
}

int command_backlinks(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_link_list_options(
      argc,
      argv,
      "Usage: holderctl backlinks <card-id> [--include-deleted] [--json]",
      "backlinks"
  );

  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    (void)fetch_card_in_current_project(paths, current_project_id, options.card_id);

    std::string target = "/cards/" + url_encode_component(options.card_id) + "/backlinks";
    if (options.include_deleted) {
      target += "?include_deleted=1";
    }
    const auto payload = card_api_request(paths, boost::beast::http::verb::get, target);
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      print_links_table(payload.at("data"), true);
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list backlinks: ") + ex.what());
  }
}

int command_link(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_link_create_options(argc, argv);

  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    (void)fetch_card_in_current_project(paths, current_project_id, options.from_card_id);

    // LCOV_EXCL_START
    nlohmann::json body = {
        {"to_card_id", options.to_card_id},
        {"to_type", "card"},
        {"kind", options.kind},
        {"created_at", now_epoch_seconds()},
    };
    // LCOV_EXCL_STOP
    if (options.label.has_value()) {
      body["label"] = options.label.value();
    }

    const auto payload = card_api_request(
        paths,
        boost::beast::http::verb::post,
        "/cards/" + url_encode_component(options.from_card_id) + "/links",
        body,
        boost::beast::http::status::created
    );
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      std::cout << "Linked card: " << options.from_card_id << " -> " << options.to_card_id << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to link cards: ") + ex.what());
  }
}

} // namespace holder::cli
