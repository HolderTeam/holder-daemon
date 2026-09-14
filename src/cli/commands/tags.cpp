#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace holder::cli {
namespace {

struct TagsOptions {
  bool json_output = false;
  std::optional<std::string> tag;
};

struct TagMutationOptions {
  bool json_output = false;
  std::string operation;
  std::string card_reference;
  std::string tag;
};

TagsOptions parse_tags_options(int argc, char* argv[]) {
  TagsOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown tags option: " + arg);
    } else if (!options.tag.has_value()) {
      options.tag = arg;
    } else {
      throw std::runtime_error("Usage: holderctl tags [TAG] [--json]");
    }
  }
  if (options.tag.has_value() && options.tag->empty()) {
    throw std::runtime_error("TAG must not be empty");
  }
  return options;
}

TagMutationOptions parse_tag_mutation_options(int argc, char* argv[]) {
  TagMutationOptions options;
  std::vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown tag option: " + arg);
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.size() != 3 || (positional.front() != "add" && positional.front() != "remove")) {
    throw std::runtime_error("Usage: holderctl tag add|remove CARD TAG [--json]");
  }
  options.operation = positional.at(0);
  options.card_reference = positional.at(1);
  options.tag = positional.at(2);
  return options;
}

nlohmann::json tag_mutation_request(
    const holder::core::Paths& paths,
    boost::beast::http::verb method,
    const std::string& card_id,
    const std::string& project_id,
    const std::string& tag
) {
  const auto connection = read_secure_daemon_connection(paths);
  const auto response = http_json_request(
      connection,
      method,
      "/cards/" + url_encode_component(card_id) + "/tags",
      std::chrono::seconds(30), // LCOV_EXCL_LINE
      nlohmann::json{{"project_id", project_id}, {"tag", tag}}
  );
  if (response.status == boost::beast::http::status::ok && response.payload.value("ok", false)) {
    return response.payload;
  }

  const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
  const auto message = api_error_message(response, fallback);
  std::string code = "tag_request_failed";
  nlohmann::json details = nlohmann::json::object();
  if (response.payload.contains("error") && response.payload.at("error").is_object()) {
    const auto& error = response.payload.at("error");
    code = json_string(error, "code", code);
    if (error.contains("details")) details = error.at("details");
  }
  throw CliError(code, message, std::move(details), message);
}

void print_tag_mutation(const nlohmann::json& data) {
  const auto card_id = json_string(data, "card_id");
  const auto tag = json_string(data, "tag");
  const auto outcome = json_string(data, "outcome");
  const auto shown_id = display_card_id(card_id);
  if (outcome == "added") {
    std::cout << "Added tag #" << tag << " to " << shown_id << ".\n";
  } else if (outcome == "already_present") {
    std::cout << "Tag #" << tag << " is already present on " << shown_id << ".\n";
  } else if (outcome == "removed") {
    std::cout << "Removed tag #" << tag << " from " << shown_id << ".\n";
  } else if (outcome == "not_present") {
    std::cout << "Tag #" << tag << " is not present on " << shown_id << ".\n";
  } else if (outcome == "present_outside_editable_tag_line") {
    std::cout << "Tag #" << tag << " was not removed from " << shown_id
              << ": it appears outside the editable trailing tag line; edit the card text "
                 "directly.\n";
  } else {
    throw std::runtime_error("Invalid tag mutation response from daemon.");
  }
}

} // namespace

int command_tags(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_tags_options(argc, argv);
  const auto project = require_current_project_payload(paths);
  const auto project_id = json_string(project, "project_id");

  try {
    std::string target;
    std::string normalized_tag;
    if (options.tag.has_value()) {
      normalized_tag = lower_ascii(options.tag.value());
      target = "/cards?project_id=" + url_encode_component(project_id) +
               "&tag=" + url_encode_component(normalized_tag);
    } else {
      target = "/projects/" + url_encode_component(project_id) + "/tags";
    }

    const auto payload = card_api_request(paths, boost::beast::http::verb::get, target);
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
      return 0;
    }

    const auto& data = payload.at("data");
    if (!data.is_array() || data.empty()) {
      if (options.tag.has_value()) {
        std::cout << "No cards tagged #" << normalized_tag << ".\n";
      } else {
        std::cout << "No tags.\n";
      }
      return 0;
    }

    if (!options.tag.has_value()) {
      std::cout << "TAG\tCARDS\n";
      for (const auto& item : data) {
        std::cout << json_string(item, "tag") << "\t" << item.value("card_count", 0) << "\n";
      }
      return 0;
    }

    std::vector<std::string> card_ids;
    card_ids.reserve(data.size());
    for (const auto& card : data)
      card_ids.push_back(json_string(card, "card_id"));
    const auto displayed_ids = display_card_ids(card_ids);
    std::cout << "CARD_ID\tTITLE\n";
    for (std::size_t i = 0; i < data.size(); ++i) {
      std::cout << displayed_ids.at(i) << "\t" << json_string(data.at(i), "title") << "\n";
    }
    return 0;
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list tags: ") + ex.what());
  }
}

int command_tag(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_tag_mutation_options(argc, argv);
  const auto project = require_current_project_payload(paths);
  const auto project_id = json_string(project, "project_id");

  try {
    const auto card_id =
        resolve_card_reference(paths, project_id, options.card_reference, CardReferenceScope::Live);
    const auto method = options.operation == "add" ? boost::beast::http::verb::post
                                                   : boost::beast::http::verb::delete_;
    const auto payload = tag_mutation_request(paths, method, card_id, project_id, options.tag);
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      print_tag_mutation(payload.at("data"));
    }
    return 0;
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to update tag: ") + ex.what());
  }
}

} // namespace holder::cli
