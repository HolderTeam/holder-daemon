#include "cli/commands/Commands.h"

#include "cli/commands/Common.h"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <optional>
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
  return fallback; // LCOV_EXCL_LINE: daemon error responses should carry error.message.
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

bool is_home_project(const nlohmann::json& project) {
  auto name = json_string(project, "name");
  for (char& c : name) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return name == "home";
}

nlohmann::json find_home_project(const nlohmann::json& projects) {
  for (const auto& project : projects) {
    if (is_home_project(project)) {
      return project;
    }
  }
  throw std::runtime_error("Default Home project not found.");
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
      throw std::runtime_error("Failed to open holderctl config: " + tmp_path); // LCOV_EXCL_LINE: requires filesystem permission fault.
    }
    out << config.dump(2) << "\n";
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, config_path, ec);
  if (ec) {
    std::filesystem::remove(config_path, ec); // LCOV_EXCL_LINE: cross-device/permission fallback is platform dependent.
    std::filesystem::rename(tmp_path, config_path, ec); // LCOV_EXCL_LINE
    if (ec) { // LCOV_EXCL_LINE
      throw std::runtime_error("Failed to write holderctl config: " + config_path.string() + // LCOV_EXCL_LINE
                               " (" + ec.message() + ")"); // LCOV_EXCL_LINE
    }
  }
} // LCOV_EXCL_LINE

void reset_holderctl_config(const holder::core::Paths& paths) {
  std::error_code ec;
  std::filesystem::remove(holderctl_config_path(paths), ec);
} // LCOV_EXCL_LINE

std::optional<std::string> read_configured_project_id(const holder::core::Paths& paths) {
  const auto config_path = holderctl_config_path(paths);
  std::ifstream in(config_path);
  if (!in.is_open()) {
    return std::nullopt;
  }
  const auto config = nlohmann::json::parse(in);
  const auto project_id = json_string(config, "current_project_id");
  if (project_id.empty()) {
    return std::nullopt;
  }
  return project_id;
} // LCOV_EXCL_LINE

std::string read_current_project_id(const holder::core::Paths& paths) {
  const auto configured_project_id = read_configured_project_id(paths);
  if (configured_project_id.has_value()) {
    return configured_project_id.value();
  }

  const auto payload = list_projects_payload(paths, false);
  return json_string(find_home_project(payload.at("data")), "project_id");
} // LCOV_EXCL_LINE

nlohmann::json find_project_by_id(const nlohmann::json& projects, const std::string& project_id) {
  for (const auto& project : projects) {
    if (json_string(project, "project_id") == project_id) {
      return project;
    }
  }
  throw std::runtime_error("Current project no longer exists: " + project_id);
} // LCOV_EXCL_LINE

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

struct CardsOptions {
  bool json_output = false;
  bool recent = false;
  int limit = 20;
  std::optional<std::string> parent_card_id;
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

struct ResourceListOptions {
  bool json_output = false;
  std::string filter;
};

struct ResourceAddOptions {
  bool json_output = false;
  std::string uri;
  std::string kind;
  std::string label;
  std::optional<std::string> desc;
};

struct ResourceEditOptions {
  bool json_output = false;
  std::string resource_id;
  std::optional<std::string> kind;
  std::optional<std::string> uri;
  std::optional<std::string> label;
  std::optional<std::string> desc;
  bool clear_desc = false;
};

std::string join_args(int start, int argc, char* argv[]) {
  std::string out;
  for (int i = start; i < argc; ++i) {
    if (!out.empty()) out += " ";
    out += argv[i];
  }
  return out;
} // LCOV_EXCL_LINE

std::string read_stdin_all() {
  std::ostringstream buffer;
  buffer << std::cin.rdbuf();
  return buffer.str();
}

std::string trim_ascii_whitespace(const std::string& value) {
  const auto start = value.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(start, end - start + 1);
}

std::string first_non_empty_line(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    const auto trimmed = trim_ascii_whitespace(line);
    if (!trimmed.empty()) {
      return trimmed;
    }
  }
  return ""; // LCOV_EXCL_LINE: command_new rejects all-whitespace content before title derivation.
}

std::string title_from_content(const std::string& content) {
  auto title = first_non_empty_line(content);
  if (title.empty()) {
    title = "Untitled"; // LCOV_EXCL_LINE: command_new rejects all-whitespace content before title derivation.
  }
  constexpr std::size_t kMaxTitleLength = 80;
  if (title.size() > kMaxTitleLength) {
    title = title.substr(0, kMaxTitleLength);
  }
  return title;
} // LCOV_EXCL_LINE

void trim_trailing_line_breaks(std::string& text) {
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
}

long long now_epoch_seconds() {
  return static_cast<long long>(std::time(nullptr));
}

std::string lower_ascii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
  return lower_ascii(haystack).find(lower_ascii(needle)) != std::string::npos;
}

bool has_url_scheme(const std::string& uri) {
  const auto scheme_pos = uri.find("://");
  return scheme_pos != std::string::npos && scheme_pos > 0;
}

std::string strip_url_query_fragment(std::string value) {
  const auto pos = value.find_first_of("?#");
  if (pos != std::string::npos) {
    value.resize(pos);
  }
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string uri_basename(const std::string& uri) {
  const auto stripped = strip_url_query_fragment(uri);
  const auto slash_pos = stripped.find_last_of("/\\");
  if (slash_pos == std::string::npos) {
    return stripped;
  }
  return stripped.substr(slash_pos + 1);
}

std::string uri_host(const std::string& uri) { // LCOV_EXCL_LINE
  // LCOV_EXCL_START: fallback for malformed URL-like inputs after basename inference.
  const auto scheme_pos = uri.find("://");
  if (scheme_pos == std::string::npos) {
    return "";
  }
  const auto host_start = scheme_pos + 3;
  const auto host_end = uri.find('/', host_start);
  return uri.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
  // LCOV_EXCL_STOP
} // LCOV_EXCL_LINE

bool has_image_extension(const std::string& uri) {
  const auto base = lower_ascii(uri_basename(uri));
  return base.ends_with(".png") || base.ends_with(".jpg") || base.ends_with(".jpeg") ||
         base.ends_with(".gif") || base.ends_with(".webp") || base.ends_with(".svg") ||
         base.ends_with(".bmp") || base.ends_with(".tif") || base.ends_with(".tiff");
} // LCOV_EXCL_LINE

std::string infer_resource_kind(const std::string& uri) {
  const auto lower = lower_ascii(uri);
  if (lower.rfind("git@", 0) == 0 || lower.ends_with(".git")) {
    return "repo";
  }
  if (has_image_extension(uri)) {
    return "image";
  }
  std::error_code ec;
  if (std::filesystem::is_directory(uri, ec)) {
    return "dir";
  }
  ec.clear();
  if (std::filesystem::is_regular_file(uri, ec)) {
    return "file";
  }
  if (has_url_scheme(uri)) {
    return "url";
  }
  if (uri.find('/') != std::string::npos || uri.find('\\') != std::string::npos) {
    return "file";
  }
  return "url";
}

std::string infer_resource_label(const std::string& uri) {
  auto label = uri_basename(uri);
  if (label.empty() && has_url_scheme(uri)) {
    label = uri_host(uri); // LCOV_EXCL_LINE: see uri_host.
  }
  if (label.empty()) {
    label = uri;
  }
  return label;
} // LCOV_EXCL_LINE

std::string shell_quote(const std::string& value) {
#ifdef _WIN32
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '"') out += '\\';
    out += ch;
  }
  out += '"';
  return out;
#else
  std::string out = "'";
  for (const char ch : value) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out += ch;
    }
  }
  out += "'";
  return out;
#endif
}

std::string sanitized_filename_component(const std::string& value) {
  std::string out;
  for (const char ch : value) {
    const auto c = static_cast<unsigned char>(ch);
    if (std::isalnum(c) || ch == '-' || ch == '_') {
      out += ch;
    } else {
      out += '_';
    }
  }
  return out.empty() ? "card" : out;
}

std::filesystem::path edit_temp_path(const holder::core::Paths& paths, const std::string& card_id) {
  const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
  return paths.cache_dir /
         ("holderctl-edit-" + sanitized_filename_component(card_id) + "-" +
          std::to_string(unique) + ".md");
}

std::string read_text_file_raw(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("Failed to read editor temp file: " + path.string()); // LCOV_EXCL_LINE: requires the editor to remove or revoke the temp file.
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_text_file_raw(const std::filesystem::path& path, const std::string& content) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    throw std::runtime_error("Failed to create editor temp file: " + path.string()); // LCOV_EXCL_LINE: requires filesystem permission fault.
  }
  out << content;
}

void remove_temp_file(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

std::string required_editor() {
  const char* editor = std::getenv("EDITOR");
  if (editor == nullptr || std::string(editor).empty()) {
    throw std::runtime_error("EDITOR is not set.");
  }
  return editor;
}

void run_editor_on_file(const std::string& editor, const std::filesystem::path& path) {
  const auto command = editor + " " + shell_quote(path.string());
  const int rc = std::system(command.c_str());
  if (rc != 0) {
    throw std::runtime_error("Editor failed.");
  }
}

std::string resource_usage() {
  return "Usage: holderctl resource <list|add|show|edit|open|delete> ...";
}

ResourceListOptions parse_resource_list_options(int argc, char* argv[]) {
  ResourceListOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--filter") {
      if (i + 1 >= argc) {
        throw std::runtime_error("Usage: holderctl resource list [--json] [--filter <query>]");
      }
      options.filter = argv[++i];
    } else {
      throw std::runtime_error("Unknown resource list option: " + arg);
    }
  }
  return options;
}

ResourceAddOptions parse_resource_add_options(int argc, char* argv[]) {
  ResourceAddOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& usage) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(usage);
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error("Resource option value must not be empty");
      }
      return value;
    };

    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--kind") {
      options.kind = require_value("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    } else if (arg == "--label") {
      options.label = require_value("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    } else if (arg == "--desc") {
      options.desc = require_value("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown resource add option: " + arg);
    } else if (options.uri.empty()) {
      options.uri = arg;
    } else {
      throw std::runtime_error("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    }
  }

  if (options.uri.empty()) {
    throw std::runtime_error("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
  }
  if (options.kind.empty()) {
    options.kind = infer_resource_kind(options.uri);
  }
  if (options.label.empty()) {
    options.label = infer_resource_label(options.uri);
  }
  return options;
}

ResourceEditOptions parse_resource_edit_options(int argc, char* argv[]) {
  ResourceEditOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& usage) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(usage);
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error("Resource option value must not be empty");
      }
      return value;
    };

    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--kind") {
      options.kind = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--uri") {
      options.uri = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--label") {
      options.label = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--desc") {
      options.desc = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--clear-desc") {
      options.clear_desc = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown resource edit option: " + arg);
    } else if (options.resource_id.empty()) {
      options.resource_id = arg;
    } else {
      throw std::runtime_error("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    }
  }

  if (options.resource_id.empty()) {
    throw std::runtime_error("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
  }
  if (options.desc.has_value() && options.clear_desc) {
    throw std::runtime_error("--desc and --clear-desc cannot be used together");
  }
  if (!options.kind.has_value() && !options.uri.has_value() && !options.label.has_value() &&
      !options.desc.has_value() && !options.clear_desc) {
    throw std::runtime_error("resource edit requires at least one field to update");
  }
  return options;
}

std::string parse_single_resource_id(int argc, char* argv[], const std::string& usage) {
  if (argc != 4) {
    throw std::runtime_error(usage);
  }
  const std::string resource_id = argv[3];
  if (resource_id.empty()) {
    throw std::runtime_error(usage);
  }
  return resource_id;
}

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

CardsOptions parse_cards_options(int argc, char* argv[]) {
  CardsOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--recent") {
      options.recent = true;
    } else if (arg == "--limit") {
      if (i + 1 >= argc) {
        throw std::runtime_error("Usage: holderctl cards [--json] [--recent [--limit N] | --parent <card-id>]");
      }
      try {
        options.limit = std::stoi(argv[++i]);
      } catch (const std::exception&) {
        throw std::runtime_error("Invalid cards limit: " + std::string(argv[i]));
      }
      if (options.limit < 1) {
        throw std::runtime_error("Cards limit must be at least 1");
      }
    } else if (arg == "--parent") {
      if (i + 1 >= argc) {
        throw std::runtime_error("Usage: holderctl cards [--json] [--recent [--limit N] | --parent <card-id>]");
      }
      const std::string parent = argv[++i];
      if (parent.empty()) {
        throw std::runtime_error("--parent must not be empty");
      }
      options.parent_card_id = parent;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown cards option: " + arg);
    } else {
      throw std::runtime_error("Usage: holderctl cards [--json] [--recent [--limit N] | --parent <card-id>]");
    }
  }

  if (options.recent && options.parent_card_id.has_value()) {
    throw std::runtime_error("holderctl cards cannot combine --recent and --parent");
  }
  if (!options.recent && options.limit != 20) {
    throw std::runtime_error("holderctl cards --limit requires --recent");
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
      throw std::runtime_error(recovery_token_usage(subcommand)); // LCOV_EXCL_LINE: parser rejects these options for export.
    }
  } else {
    const bool has_file = !options.in_path.empty();
    const bool has_token = !options.token.empty();
    if (has_file == has_token) {
      throw std::runtime_error(recovery_token_usage(subcommand));
    }
    if (!options.out_path.empty()) {
      throw std::runtime_error(recovery_token_usage(subcommand)); // LCOV_EXCL_LINE: parser rejects --out for import subcommands.
    }
  }

  return options;
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

nlohmann::json require_current_project_payload(const holder::core::Paths& paths) {
  const auto current_project_id = read_current_project_id(paths);
  const auto projects_payload = list_projects_payload(paths, false);
  return find_project_by_id(projects_payload.at("data"), current_project_id);
}

nlohmann::json card_api_request(const holder::core::Paths& paths,
                                boost::beast::http::verb method,
                                const std::string& target,
                                const nlohmann::json& body = nlohmann::json::object(),
                                boost::beast::http::status success = boost::beast::http::status::ok) {
  const auto connection = read_secure_daemon_connection(paths);
  const auto response = method == boost::beast::http::verb::get
                            ? http_json_request(connection, method, target, std::chrono::seconds(10)) // LCOV_EXCL_LINE: gcov misattributes the covered ternary arm.
                            : http_json_request(connection, method, target, std::chrono::seconds(30), body);

  if (response.status != success || !response.payload.value("ok", false)) {
    const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status)); // LCOV_EXCL_LINE: covered failures carry structured messages.
    throw std::runtime_error(api_error_message(response, fallback)); // LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE

  return response.payload;
}

nlohmann::json list_current_project_resources_payload(const holder::core::Paths& paths) {
  const auto project = require_current_project_payload(paths);
  const auto project_id = json_string(project, "project_id");
  return card_api_request(paths,
                          boost::beast::http::verb::get,
                          "/resources?project_id=" + url_encode_component(project_id));
}

nlohmann::json find_resource_in_payload(const nlohmann::json& resources,
                                        const std::string& resource_id) {
  for (const auto& resource : resources) {
    if (json_string(resource, "resource_id") == resource_id) {
      return resource;
    }
  }
  throw std::runtime_error("Resource not found in current project: " + resource_id);
}

bool resource_matches_filter(const nlohmann::json& resource, const std::string& filter) {
  if (filter.empty()) {
    return true; // LCOV_EXCL_LINE: callers only invoke this helper with non-empty filters.
  }
  const std::string haystack = json_string(resource, "label") + " " +
                              json_string(resource, "kind") + " " +
                              json_string(resource, "uri") + " " +
                              json_string(resource, "desc");
  return contains_case_insensitive(haystack, filter);
}

void print_resource_row(const nlohmann::json& resource) {
  std::cout << json_string(resource, "resource_id") << "\t"
            << json_string(resource, "kind") << "\t"
            << json_string(resource, "label") << "\t"
            << json_string(resource, "uri") << "\n";
}

void open_resource_uri(const std::string& uri) {
#if defined(__linux__)
  const auto opener = boost::process::v2::environment::find_executable("xdg-open");
  if (opener.empty()) {
    std::cout << uri << "\n"; // LCOV_EXCL_LINE: depends on host PATH contents.
    throw std::runtime_error("xdg-open not found"); // LCOV_EXCL_LINE
  }

  boost::asio::io_context ioc;
  boost::process::v2::process proc(ioc.get_executor(), opener, {uri});

  boost::system::error_code ec;
  const int exit_code = proc.wait(ec);
  if (ec) {
    std::cout << uri << "\n"; // LCOV_EXCL_LINE: requires process wait syscall failure.
    throw std::runtime_error("Failed to run xdg-open: " + ec.message()); // LCOV_EXCL_LINE
  }
  if (exit_code != 0) {
    std::cout << uri << "\n";
    throw std::runtime_error("xdg-open failed with exit code " + std::to_string(exit_code));
  }
#else
  std::cout << uri << "\n";
  throw std::runtime_error("resource open is not supported on this platform yet"); // LCOV_EXCL_LINE
#endif
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
  const auto payload = list_projects_payload(paths, false);
  if (argc == 2) {
    const auto project = find_home_project(payload.at("data"));
    reset_holderctl_config(paths);
    std::cout << "Current project: " << json_string(project, "name") << " ("
              << json_string(project, "project_id") << ")\n";
    return 0;
  }

  if (argc != 3) {
    throw std::runtime_error("Usage: holderctl use <project-id-or-name>");
  }

  const std::string query = argv[2];
  const auto project = resolve_project(payload.at("data"), query);
  const auto project_id = json_string(project, "project_id");
  if (is_home_project(project)) {
    reset_holderctl_config(paths);
  } else {
    write_holderctl_config(paths, project_id);
  }

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

int command_cards(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_cards_options(argc, argv);
  const auto project = require_current_project_payload(paths);
  const auto project_id = json_string(project, "project_id");

  try {
    std::string target = "/cards?project_id=" + url_encode_component(project_id) + "&count=true";
    if (options.recent) {
      target += "&view=recent&limit=" + std::to_string(options.limit);
    } else {
      target += "&view=tree";
      if (options.parent_card_id.has_value()) {
        target += "&parent_card_id=" + url_encode_component(options.parent_card_id.value());
      }
    }

    const auto payload = card_api_request(paths, boost::beast::http::verb::get, target);
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
      return 0;
    }

    const auto& cards = payload.at("data");
    if (!cards.is_array() || cards.empty()) {
      std::cout << (options.recent ? "No recent cards.\n" : "No root cards.\n");
      return 0;
    }

    std::cout << "CARD_ID\tTITLE\tCHILDREN\tUPDATED\n";
    for (const auto& card : cards) {
      std::cout << json_string(card, "card_id") << "\t"
                << json_string(card, "title") << "\t"
                << card.value("child_count", 0) << "\t"
                << card.value("updated_at", 0) << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list cards: ") + ex.what());
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

int command_edit(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc != 3 || std::string(argv[2]).empty()) {
    throw std::runtime_error("Usage: holderctl edit <card-id>");
  }

  const std::string card_id = argv[2];
  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    const auto fetched = card_api_request(paths,
                                          boost::beast::http::verb::get,
                                          "/cards/" + url_encode_component(card_id));
    const auto& data = fetched.at("data");
    if (json_string(data, "project_id") != current_project_id) {
      throw std::runtime_error("Card is not in the current project: " + card_id);
    }

    const auto editor = required_editor();
    const auto original_content = json_string(data, "content");
    const auto temp_path = edit_temp_path(paths, card_id);
    write_text_file_raw(temp_path, original_content);

    run_editor_on_file(editor, temp_path);
    const auto edited_content = read_text_file_raw(temp_path);

    if (edited_content == original_content) {
      remove_temp_file(temp_path);
      std::cout << "No changes.\n";
      return 0;
    }

    (void)card_api_request(paths,
                           boost::beast::http::verb::patch,
                           "/cards/" + url_encode_component(card_id),
                           {{"content", edited_content},
                            {"title", json_string(data, "title")},
                            {"updated_at", now_epoch_seconds()}}); // LCOV_EXCL_LINE: gcov misattributes covered JSON initializer line.
    remove_temp_file(temp_path);
    std::cout << "Updated card: " << card_id << "\n";
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to edit card: ") + ex.what());
  }
}

int command_new(const holder::core::Paths& paths, int argc, char* argv[]) {
  std::string content = join_args(2, argc, argv);
  if (content.empty()) {
    content = read_stdin_all();
  }
  if (trim_ascii_whitespace(content).empty()) {
    throw std::runtime_error("Usage: holderctl new <text>  OR  <command> | holderctl new");
  }

  try {
    const auto project = require_current_project_payload(paths);
    const auto project_id = json_string(project, "project_id");
    const auto payload = card_api_request(paths,
                                          boost::beast::http::verb::post,
                                          "/cards",
                                          {{"project_id", project_id},
                                           {"title", title_from_content(content)},
                                           {"content", content},
                                           {"created_at", now_epoch_seconds()}, // LCOV_EXCL_LINE: gcov misattributes covered JSON initializer lines.
                                           {"updated_at", now_epoch_seconds()}}, // LCOV_EXCL_LINE
                                          boost::beast::http::status::created);
    std::cout << "Created card: " << json_string(payload.at("data"), "card_id") << "\n";
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to create card: ") + ex.what());
  }
}

int command_append(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error("Usage: holderctl append <card-id> <text>  OR  <command> | holderctl append <card-id>");
  }

  const std::string card_id = argv[2];
  std::string addition = join_args(3, argc, argv);
  if (addition.empty()) {
    addition = read_stdin_all();
  }
  if (trim_ascii_whitespace(addition).empty()) {
    throw std::runtime_error("Usage: holderctl append <card-id> <text>  OR  <command> | holderctl append <card-id>");
  }

  try {
    const auto project = require_current_project_payload(paths);
    const auto current_project_id = json_string(project, "project_id");
    const auto fetched = card_api_request(paths,
                                          boost::beast::http::verb::get,
                                          "/cards/" + url_encode_component(card_id));
    const auto& data = fetched.at("data");
    if (json_string(data, "project_id") != current_project_id) {
      throw std::runtime_error("Card is not in the current project: " + card_id);
    }

    auto content = json_string(data, "content");
    trim_trailing_line_breaks(content);
    if (!content.empty()) {
      content += "\n\n";
    }
    content += addition;

    (void)card_api_request(paths,
                           boost::beast::http::verb::patch,
                           "/cards/" + url_encode_component(card_id),
                           {{"content", content},
                            {"title", json_string(data, "title")},
                            {"updated_at", now_epoch_seconds()}}); // LCOV_EXCL_LINE: gcov misattributes covered JSON initializer line.
    std::cout << "Appended to card: " << card_id << "\n";
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to append to card: ") + ex.what());
  }
}

int command_resource(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(resource_usage());
  }

  const std::string subcommand = argv[2];
  try {
    if (subcommand == "list") {
      const auto options = parse_resource_list_options(argc, argv);
      auto payload = list_current_project_resources_payload(paths);
      if (options.filter.empty()) {
        if (options.json_output) {
          std::cout << payload.dump(2) << "\n";
          return 0;
        }
      } else {
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& resource : payload.at("data")) {
          if (resource_matches_filter(resource, options.filter)) {
            filtered.push_back(resource);
          }
        }
        payload["data"] = std::move(filtered);
        if (options.json_output) {
          std::cout << payload.dump(2) << "\n";
          return 0;
        }
      }

      const auto& resources = payload.at("data");
      if (!resources.is_array() || resources.empty()) {
        std::cout << "No resources.\n";
        return 0;
      }

      std::cout << "RESOURCE_ID\tKIND\tLABEL\tURI\n";
      for (const auto& resource : resources) {
        print_resource_row(resource);
      }
      return 0;
    }

    if (subcommand == "add") {
      const auto options = parse_resource_add_options(argc, argv);
      const auto project = require_current_project_payload(paths);
      const auto project_id = json_string(project, "project_id");
      nlohmann::json body = {
          {"project_id", project_id},
          {"kind", options.kind},
          {"uri", options.uri},
          {"label", options.label},
          {"created_at", now_epoch_seconds()}, // LCOV_EXCL_LINE: gcov misattributes covered JSON initializer lines.
          {"updated_at", now_epoch_seconds()}, // LCOV_EXCL_LINE
      };
      if (options.desc.has_value()) {
        body["desc"] = options.desc.value();
      }
      const auto payload = card_api_request(paths,
                                            boost::beast::http::verb::post,
                                            "/resources",
                                            body,
                                            boost::beast::http::status::created);
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        std::cout << "Created resource: " << json_string(payload.at("data"), "resource_id") << "\n";
      }
      return 0;
    }

    if (subcommand == "show") {
      bool json_output = false;
      int resource_arg_index = 3;
      if (argc >= 4 && std::string(argv[3]) == "--json") {
        json_output = true;
        resource_arg_index = 4;
      }
      if (argc != resource_arg_index + 1) {
        throw std::runtime_error("Usage: holderctl resource show [--json] <resource-id>");
      }
      const std::string resource_id = argv[resource_arg_index];
      const auto payload = list_current_project_resources_payload(paths);
      const auto resource = find_resource_in_payload(payload.at("data"), resource_id);
      if (json_output) {
        nlohmann::json out;
        out["ok"] = true;
        out["data"] = resource;
        std::cout << out.dump(2) << "\n";
      } else {
        std::cout << "Resource: " << json_string(resource, "resource_id") << "\n"
                  << "Kind: " << json_string(resource, "kind") << "\n"
                  << "Label: " << json_string(resource, "label") << "\n"
                  << "URI: " << json_string(resource, "uri") << "\n";
        const auto desc = json_string(resource, "desc");
        if (!desc.empty()) {
          std::cout << "Desc: " << desc << "\n";
        }
      }
      return 0;
    }

    if (subcommand == "edit") {
      const auto options = parse_resource_edit_options(argc, argv);
      const auto resources_payload = list_current_project_resources_payload(paths);
      (void)find_resource_in_payload(resources_payload.at("data"), options.resource_id);

      nlohmann::json body;
      body["updated_at"] = now_epoch_seconds();
      if (options.kind.has_value()) body["kind"] = options.kind.value();
      if (options.uri.has_value()) body["uri"] = options.uri.value();
      if (options.label.has_value()) body["label"] = options.label.value();
      if (options.clear_desc) {
        body["desc"] = nullptr;
      } else if (options.desc.has_value()) {
        body["desc"] = options.desc.value();
      }

      const auto payload = card_api_request(paths,
                                            boost::beast::http::verb::patch,
                                            "/resources/" + url_encode_component(options.resource_id),
                                            body);
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        std::cout << "Updated resource: " << options.resource_id << "\n";
      }
      return 0;
    }

    if (subcommand == "open") {
      const auto resource_id = parse_single_resource_id(argc, argv, "Usage: holderctl resource open <resource-id>");
      const auto payload = list_current_project_resources_payload(paths);
      const auto resource = find_resource_in_payload(payload.at("data"), resource_id);
      const auto uri = json_string(resource, "uri");
      if (uri.empty()) {
        throw std::runtime_error("Resource has no URI: " + resource_id);
      }
      open_resource_uri(uri);
      return 0;
    }

    if (subcommand == "delete") {
      const auto resource_id = parse_single_resource_id(argc, argv, "Usage: holderctl resource delete <resource-id>");
      const auto resources_payload = list_current_project_resources_payload(paths);
      (void)find_resource_in_payload(resources_payload.at("data"), resource_id);
      (void)card_api_request(paths,
                             boost::beast::http::verb::delete_,
                             "/resources/" + url_encode_component(resource_id));
      std::cout << "Deleted resource: " << resource_id << "\n";
      return 0;
    }
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Resource command failed: ") + ex.what());
  }

  throw std::runtime_error(resource_usage());
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
        throw std::runtime_error("Recovery token export response did not include a token"); // LCOV_EXCL_LINE: protocol violation.
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
              << "Project created: " << (data.value("project_created", false) ? "yes" : "no") << "\n" // LCOV_EXCL_LINE: gcov misattributes covered ostream chain lines.
              << "Remote hint: " << (data.value("remote_hint_present", false) ? "yes" : "no") << "\n" // LCOV_EXCL_LINE
              << "Remote configured: " << (data.value("remote_configured", false) ? "yes" : "no") << "\n" // LCOV_EXCL_LINE
              << "Pull status: " << json_string(data, "pull_status", "not_attempted") << "\n";
    if (data.contains("remote_error") && !data.at("remote_error").is_null()) {
      std::cout << "Remote error: " << data.at("remote_error").get<std::string>() << "\n"; // LCOV_EXCL_LINE: depends on optional git remote failure.
    }
    if (data.contains("pull_error") && !data.at("pull_error").is_null()) {
      std::cout << "Pull error: " << data.at("pull_error").get<std::string>() << "\n"; // LCOV_EXCL_LINE: depends on optional git pull failure.
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Recovery token command failed: ") + ex.what());
  }
}

} // namespace holder::cli
