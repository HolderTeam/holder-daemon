#include "cli/commands/Support.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace holder::cli {

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
      connection,
      boost::beast::http::verb::get,
      target,
      std::chrono::seconds(10) // LCOV_EXCL_LINE
  );

  if (response.status != boost::beast::http::status::ok || !response.payload.value("ok", false)) {
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
      // LCOV_EXCL_START
      throw std::runtime_error("Failed to open holderctl config: " + tmp_path);
      // LCOV_EXCL_STOP
    }
    out << config.dump(2) << "\n";
  }

  std::error_code ec;
  std::filesystem::rename(tmp_path, config_path, ec);
  if (ec) {
    // LCOV_EXCL_START
    std::filesystem::remove(config_path, ec);
    std::filesystem::rename(tmp_path, config_path, ec);
    if (ec) {
      throw std::runtime_error(
          "Failed to write holderctl config: " + config_path.string() + " (" + ec.message() + ")"
      );
    }
    // LCOV_EXCL_STOP
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
  auto project_id = json_string(config, "current_project_id");
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
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_' || c == '.' || c == '~';
    if (safe) {
      out << static_cast<char>(c);
    } else {
      out << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(c);
    }
  }
  return out.str();
}

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

namespace {

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

} // namespace

std::string title_from_content(const std::string& content) {
  auto title = first_non_empty_line(content);
  if (title.empty()) {
    title = "Untitled"; // LCOV_EXCL_LINE: command_new rejects all-whitespace content before title
                        // derivation.
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

long long now_epoch_seconds() { return static_cast<long long>(std::time(nullptr)); }

std::string lower_ascii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool contains_case_insensitive(const std::string& haystack, const std::string& needle) {
  return lower_ascii(haystack).find(lower_ascii(needle)) != std::string::npos;
}

nlohmann::json recovery_token_request(
    const holder::core::Paths& paths,
    boost::beast::http::verb method,
    const std::string& target,
    const nlohmann::json& body
) {
  const auto connection = read_secure_daemon_connection(paths);
  const auto response = http_json_request(
      connection,
      method,
      target,
      std::chrono::seconds(30), // LCOV_EXCL_LINE: exercised through generic JSON request helpers.
      std::optional<nlohmann::json>{body}
  );

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

nlohmann::json card_api_request(
    const holder::core::Paths& paths,
    boost::beast::http::verb method,
    const std::string& target,
    const nlohmann::json& body,
    boost::beast::http::status success
) {
  const auto connection = read_secure_daemon_connection(paths);
  const auto response = method == boost::beast::http::verb::get
                            ? http_json_request(
                                  connection,
                                  method,
                                  target,
                                  std::chrono::seconds(10) // LCOV_EXCL_LINE
                              )
                            : http_json_request(
                                  connection,
                                  method,
                                  target,
                                  std::chrono::seconds(30), // LCOV_EXCL_LINE
                                  std::optional<nlohmann::json>{body}
                              );

  if (response.status != success || !response.payload.value("ok", false)) {
    const auto fallback = "HTTP " +
                          std::to_string(static_cast<unsigned>(response.status)
                          ); // LCOV_EXCL_LINE: covered failures carry structured messages.
    throw std::runtime_error(api_error_message(response, fallback)); // LCOV_EXCL_LINE
  } // LCOV_EXCL_LINE

  return response.payload;
}

nlohmann::json fetch_card_in_current_project(
    const holder::core::Paths& paths,
    const std::string& current_project_id,
    const std::string& card_id
) {
  const auto payload = card_api_request(
      paths,
      boost::beast::http::verb::get,
      "/cards/" + url_encode_component(card_id)
  );
  const auto& data = payload.at("data");
  if (json_string(data, "project_id") != current_project_id) {
    throw std::runtime_error("Card is not in the current project: " + card_id);
  }
  return payload;
}

} // namespace holder::cli
