#include "cli/commands/Commands.h"

#include "cli/commands/Common.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace holder::cli {

namespace {

std::string api_error_message(const HttpJsonResponse& response, const std::string& fallback) {
  if (response.payload.contains("error") && response.payload["error"].contains("message")) {
    return response.payload["error"]["message"].get<std::string>();
  }
  return fallback;
}

nlohmann::json list_projects_payload(const holder::core::Paths& paths, bool include_count) {
  const auto connection = read_secure_daemon_connection(paths);
  const std::string target = include_count ? "/projects?count=true" : "/projects";
  const auto response = http_json_request(
      connection, boost::beast::http::verb::get, target, std::chrono::seconds(10));

  if (response.status != boost::beast::http::status::ok ||
      !response.payload.value("ok", false)) {
    const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
    throw std::runtime_error("Projects request failed: " + api_error_message(response, fallback));
  }

  return response.payload;
}

std::filesystem::path holderctl_config_path(const holder::core::Paths& paths) {
  return paths.config_dir / "holderctl.json";
}

void write_holderctl_config(const holder::core::Paths& paths, const std::string& project_id) {
  std::filesystem::create_directories(paths.config_dir);
  const auto config_path = holderctl_config_path(paths);
  const auto tmp_path = config_path.string() + ".tmp";

  nlohmann::json config;
  config["current_project_id"] = project_id;
  {
    std::ofstream out(tmp_path, std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("Failed to open holderctl config: " + tmp_path);
    }
    out << config.dump(2) << "\n";
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, config_path, ec);
  if (ec) {
    std::filesystem::remove(config_path, ec);
    std::filesystem::rename(tmp_path, config_path, ec);
    if (ec) {
      throw std::runtime_error("Failed to write holderctl config: " + config_path.string() +
                               " (" + ec.message() + ")");
    }
  }
}

std::string read_current_project_id(const holder::core::Paths& paths) {
  const auto config_path = holderctl_config_path(paths);
  std::ifstream in(config_path);
  if (!in.is_open()) {
    throw std::runtime_error("No current project set. Run: holderctl use <project>");
  }
  const auto config = nlohmann::json::parse(in);
  const auto project_id = json_string(config, "current_project_id");
  if (project_id.empty()) {
    throw std::runtime_error("No current project set. Run: holderctl use <project>");
  }
  return project_id;
}

nlohmann::json find_project_by_id(const nlohmann::json& projects, const std::string& project_id) {
  for (const auto& project : projects) {
    if (json_string(project, "project_id") == project_id) {
      return project;
    }
  }
  throw std::runtime_error("Current project no longer exists: " + project_id);
}

nlohmann::json resolve_project(const nlohmann::json& projects, const std::string& query) {
  std::vector<nlohmann::json> name_matches;
  for (const auto& project : projects) {
    if (json_string(project, "project_id") == query) {
      return project;
    }
    if (json_string(project, "name") == query) {
      name_matches.push_back(project);
    }
  }

  if (name_matches.size() == 1) {
    return name_matches.front();
  }
  if (name_matches.size() > 1) {
    throw std::runtime_error("Multiple projects named '" + query + "'; use the project id.");
  }
  throw std::runtime_error("Project not found: " + query);
}

std::string url_encode_component(const std::string& value) {
  std::ostringstream out;
  out << std::uppercase << std::hex;
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out << static_cast<char>(c);
    } else {
      out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
  }
  return out.str();
}

struct SearchOptions {
  bool json_output = false;
  int limit = 20;
  std::string query;
};

SearchOptions parse_search_options(int argc, char* argv[]) {
  SearchOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--limit") {
      if (i + 1 >= argc) {
        throw std::runtime_error("Usage: holderctl search [--json] [--limit N] <query>");
      }
      try {
        options.limit = std::stoi(argv[++i]);
      } catch (const std::exception&) {
        throw std::runtime_error("Invalid search limit: " + std::string(argv[i]));
      }
      if (options.limit < 1) {
        throw std::runtime_error("Search limit must be at least 1");
      }
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown search option: " + arg);
    } else {
      if (!options.query.empty()) options.query += " ";
      options.query += arg;
    }
  }

  if (options.query.empty()) {
    throw std::runtime_error("Usage: holderctl search [--json] [--limit N] <query>");
  }
  return options;
}

} // namespace

int command_reindex(const holder::core::Paths& paths, int argc) {
  if (argc > 2) {
    throw std::runtime_error("reindex does not take options");
  }

  try {
    const auto connection = read_secure_daemon_connection(paths);
    const auto response = http_json_request(
        connection, boost::beast::http::verb::post, "/reindex", std::chrono::seconds(30));

    if (response.status != boost::beast::http::status::ok ||
        !response.payload.value("ok", false)) {
      const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
      throw std::runtime_error("Reindex failed: " + api_error_message(response, fallback));
    }

    std::cout << response.payload.value("data", nlohmann::json::object())
                     .value("message", std::string{"Reindex complete."})
              << "\n";
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to request reindex: ") + ex.what());
  }
}

int command_projects(const holder::core::Paths& paths, int argc, char* argv[]) {
  bool json_output = false;
  bool include_count = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      json_output = true;
    } else if (arg == "--count") {
      include_count = true;
    } else {
      throw std::runtime_error("Unknown projects option: " + arg);
    }
  }

  try {
    const auto payload = list_projects_payload(paths, include_count);

    if (json_output) {
      std::cout << payload.dump(2) << "\n";
      return 0;
    }

    const auto& projects = payload.at("data");
    if (!projects.is_array() || projects.empty()) {
      std::cout << "No projects.\n";
      return 0;
    }

    if (include_count) {
      std::cout << "PROJECT_ID\tNAME\tCARDS\tROOT_CARDS\tROOT\n";
    } else {
      std::cout << "PROJECT_ID\tNAME\tROOT\n";
    }
    for (const auto& project : projects) {
      std::cout << json_string(project, "project_id") << "\t"
                << json_string(project, "name") << "\t";
      if (include_count) {
        std::cout << project.value("card_count", 0) << "\t"
                  << project.value("root_card_count", 0) << "\t";
      }
      std::cout << json_string(project, "root_path") << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list projects: ") + ex.what());
  }
}

int command_use(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc != 3) {
    throw std::runtime_error("Usage: holderctl use <project-id-or-name>");
  }

  const std::string query = argv[2];
  const auto payload = list_projects_payload(paths, false);
  const auto project = resolve_project(payload.at("data"), query);
  const auto project_id = json_string(project, "project_id");
  write_holderctl_config(paths, project_id);

  std::cout << "Current project: " << json_string(project, "name") << " (" << project_id << ")\n";
  return 0;
}

int command_current(const holder::core::Paths& paths, int argc) {
  if (argc != 2) {
    throw std::runtime_error("current does not take options");
  }

  const auto current_project_id = read_current_project_id(paths);
  const auto payload = list_projects_payload(paths, false);
  const auto project = find_project_by_id(payload.at("data"), current_project_id);

  std::cout << "Current project: " << json_string(project, "name") << " ("
            << current_project_id << ")\n"
            << "Root: " << json_string(project, "root_path") << "\n";
  return 0;
}

int command_search(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_search_options(argc, argv);
  const auto current_project_id = read_current_project_id(paths);
  const auto projects_payload = list_projects_payload(paths, false);
  (void)find_project_by_id(projects_payload.at("data"), current_project_id);

  try {
    const auto connection = read_secure_daemon_connection(paths);
    const std::string target = "/search/cards?project_id=" +
                               url_encode_component(current_project_id) + "&q=" +
                               url_encode_component(options.query) + "&limit=" +
                               std::to_string(options.limit);
    const auto response = http_json_request(
        connection, boost::beast::http::verb::get, target, std::chrono::seconds(10));

    if (response.status != boost::beast::http::status::ok ||
        !response.payload.value("ok", false)) {
      const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
      throw std::runtime_error("Search failed: " + api_error_message(response, fallback));
    }

    if (options.json_output) {
      std::cout << response.payload.dump(2) << "\n";
      return 0;
    }

    const auto& cards = response.payload.at("data");
    if (!cards.is_array() || cards.empty()) {
      std::cout << "No cards found.\n";
      return 0;
    }

    for (const auto& card : cards) {
      std::cout << json_string(card, "card_id") << "\t" << json_string(card, "title") << "\n";
      const auto snippet = json_string(card, "snippet");
      if (!snippet.empty()) {
        std::cout << "  " << snippet << "\n";
      }
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to search cards: ") + ex.what());
  }
}

} // namespace holder::cli
