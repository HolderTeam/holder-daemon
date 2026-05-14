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

#ifndef _WIN32
#include <sys/stat.h>
#endif

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

struct CardOptions {
  bool json_output = false;
  std::string card_id;
};

struct RecoveryTokenOptions {
  std::string pin;
  std::filesystem::path out_path;
  std::filesystem::path in_path;
  std::string token;
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

CardOptions parse_card_options(int argc, char* argv[]) {
  CardOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown card option: " + arg);
    } else if (options.card_id.empty()) {
      options.card_id = arg;
    } else {
      throw std::runtime_error("Usage: holderctl card [--json] <card-id>");
    }
  }

  if (options.card_id.empty()) {
    throw std::runtime_error("Usage: holderctl card [--json] <card-id>");
  }
  return options;
}

std::string recovery_token_usage(const std::string& subcommand) {
  if (subcommand == "export") {
    return "Usage: holderctl recovery-token export --pin <pin> [--out <file>]";
  }
  if (subcommand == "import") {
    return "Usage: holderctl recovery-token import --pin <pin> (--file <file> | --token <token>)";
  }
  if (subcommand == "import-global") {
    return "Usage: holderctl recovery-token import-global --pin <pin> (--file <file> | --token <token>)";
  }
  return "Usage: holderctl recovery-token <export|import|import-global> ...";
}

RecoveryTokenOptions parse_recovery_token_options(const std::string& subcommand,
                                                  int argc,
                                                  char* argv[]) {
  RecoveryTokenOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& option) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(recovery_token_usage(subcommand));
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error(option + " must not be empty");
      }
      return value;
    };

    if (arg == "--pin") {
      options.pin = require_value(arg);
    } else if (arg == "--out" && subcommand == "export") {
      options.out_path = require_value(arg);
    } else if (arg == "--file" && (subcommand == "import" || subcommand == "import-global")) {
      options.in_path = require_value(arg);
    } else if (arg == "--token" && (subcommand == "import" || subcommand == "import-global")) {
      options.token = require_value(arg);
    } else {
      throw std::runtime_error("Unknown recovery-token option: " + arg);
    }
  }

  if (options.pin.empty()) {
    throw std::runtime_error(recovery_token_usage(subcommand));
  }

  if (subcommand == "export") {
    if (!options.in_path.empty() || !options.token.empty()) {
      throw std::runtime_error(recovery_token_usage(subcommand));
    }
  } else {
    const bool has_file = !options.in_path.empty();
    const bool has_token = !options.token.empty();
    if (has_file == has_token) {
      throw std::runtime_error(recovery_token_usage(subcommand));
    }
    if (!options.out_path.empty()) {
      throw std::runtime_error(recovery_token_usage(subcommand));
    }
  }

  return options;
}

std::string trim_ascii_whitespace(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string read_text_file_trimmed(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("Failed to open recovery token file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const auto token = trim_ascii_whitespace(buffer.str());
  if (token.empty()) {
    throw std::runtime_error("Recovery token file is empty: " + path.string());
  }
  return token;
}

void write_recovery_token_file(const std::filesystem::path& path, const std::string& token) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("Failed to open recovery token output file: " + path.string());
    }
    out << token << "\n";
  }
#ifndef _WIN32
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

nlohmann::json recovery_token_request(const holder::core::Paths& paths,
                                      boost::beast::http::verb method,
                                      const std::string& target,
                                      const nlohmann::json& body) {
  const auto connection = read_secure_daemon_connection(paths);
  const auto response = http_json_request(
      connection, method, target, std::chrono::seconds(30), body);

  if ((response.status != boost::beast::http::status::ok &&
       response.status != boost::beast::http::status::created) ||
      !response.payload.value("ok", false)) {
    const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
    throw std::runtime_error(api_error_message(response, fallback));
  }

  return response.payload;
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

int command_card(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_card_options(argc, argv);
  const auto current_project_id = read_current_project_id(paths);
  const auto projects_payload = list_projects_payload(paths, false);
  (void)find_project_by_id(projects_payload.at("data"), current_project_id);

  try {
    const auto connection = read_secure_daemon_connection(paths);
    const auto response = http_json_request(connection,
                                            boost::beast::http::verb::get,
                                            "/cards/" + url_encode_component(options.card_id),
                                            std::chrono::seconds(10));

    if (response.status != boost::beast::http::status::ok ||
        !response.payload.value("ok", false)) {
      const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
      throw std::runtime_error("Card request failed: " + api_error_message(response, fallback));
    }

    const auto& data = response.payload.at("data");
    const auto card_project_id = json_string(data, "project_id");
    if (card_project_id != current_project_id) {
      throw std::runtime_error("Card is not in the current project: " + options.card_id);
    }

    if (options.json_output) {
      std::cout << response.payload.dump(2) << "\n";
      return 0;
    }

    const auto content = json_string(data, "content");
    std::cout << content;
    if (content.empty() || content.back() != '\n') {
      std::cout << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to print card: ") + ex.what());
  }
}

int command_recovery_token(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(recovery_token_usage(""));
  }

  const std::string subcommand = argv[2];
  if (subcommand != "export" && subcommand != "import" && subcommand != "import-global") {
    throw std::runtime_error(recovery_token_usage(""));
  }

  const auto options = parse_recovery_token_options(subcommand, argc, argv);
  try {
    if (subcommand == "export") {
      const auto current_project_id = read_current_project_id(paths);
      const auto projects_payload = list_projects_payload(paths, false);
      (void)find_project_by_id(projects_payload.at("data"), current_project_id);

      const auto payload = recovery_token_request(
          paths,
          boost::beast::http::verb::post,
          "/projects/" + url_encode_component(current_project_id) + "/recovery-token/export",
          {{"pin", options.pin}});
      const auto token = json_string(payload.at("data"), "recovery_token");
      if (token.empty()) {
        throw std::runtime_error("Recovery token export response did not include a token");
      }

      if (!options.out_path.empty()) {
        write_recovery_token_file(options.out_path, token);
        std::cout << "Recovery token exported: " << options.out_path.string() << "\n"
                  << "Project: " << current_project_id << "\n";
      } else {
        std::cout << token << "\n";
      }
      return 0;
    }

    const std::string token = !options.token.empty()
                                  ? trim_ascii_whitespace(options.token)
                                  : read_text_file_trimmed(options.in_path);
    if (token.empty()) {
      throw std::runtime_error("Recovery token must not be empty");
    }

    if (subcommand == "import") {
      const auto current_project_id = read_current_project_id(paths);
      const auto projects_payload = list_projects_payload(paths, false);
      (void)find_project_by_id(projects_payload.at("data"), current_project_id);

      const auto payload = recovery_token_request(
          paths,
          boost::beast::http::verb::post,
          "/projects/" + url_encode_component(current_project_id) + "/recovery-token/import",
          {{"pin", options.pin}, {"recovery_token", token}});
      std::cout << "Recovery token imported for project: "
                << json_string(payload.at("data"), "project_id", current_project_id) << "\n";
      return 0;
    }

    const auto payload = recovery_token_request(paths,
                                                boost::beast::http::verb::post,
                                                "/recovery-token/import",
                                                {{"pin", options.pin}, {"recovery_token", token}});
    const auto& data = payload.at("data");
    std::cout << "Recovery token imported for project: " << json_string(data, "project_id") << "\n"
              << "Project created: " << (data.value("project_created", false) ? "yes" : "no") << "\n"
              << "Remote hint: " << (data.value("remote_hint_present", false) ? "yes" : "no") << "\n"
              << "Remote configured: " << (data.value("remote_configured", false) ? "yes" : "no") << "\n"
              << "Pull status: " << json_string(data, "pull_status", "not_attempted") << "\n";
    if (data.contains("remote_error") && !data.at("remote_error").is_null()) {
      std::cout << "Remote error: " << data.at("remote_error").get<std::string>() << "\n";
    }
    if (data.contains("pull_error") && !data.at("pull_error").is_null()) {
      std::cout << "Pull error: " << data.at("pull_error").get<std::string>() << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Recovery token command failed: ") + ex.what());
  }
}

} // namespace holder::cli
