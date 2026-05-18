#pragma once

#include "cli/commands/Common.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>

namespace holder::cli {

std::string api_error_message(const HttpJsonResponse& response, const std::string& fallback);
nlohmann::json list_projects_payload(const holder::core::Paths& paths, bool include_count);

std::filesystem::path holderctl_config_path(const holder::core::Paths& paths);
bool is_home_project(const nlohmann::json& project);
nlohmann::json find_home_project(const nlohmann::json& projects);
void write_holderctl_config(const holder::core::Paths& paths, const std::string& project_id);
void reset_holderctl_config(const holder::core::Paths& paths);
std::optional<std::string> read_configured_project_id(const holder::core::Paths& paths);
std::string read_current_project_id(const holder::core::Paths& paths);
nlohmann::json find_project_by_id(const nlohmann::json& projects, const std::string& project_id);
nlohmann::json resolve_project(const nlohmann::json& projects, const std::string& query);

std::string url_encode_component(const std::string& value);
std::string join_args(int start, int argc, char* argv[]);
std::string read_stdin_all();
std::string trim_ascii_whitespace(const std::string& value);
std::string title_from_content(const std::string& content);
void trim_trailing_line_breaks(std::string& text);
long long now_epoch_seconds();
std::string lower_ascii(std::string value);
bool contains_case_insensitive(const std::string& haystack, const std::string& needle);

nlohmann::json recovery_token_request(
    const holder::core::Paths& paths,
    boost::beast::http::verb method,
    const std::string& target,
    const nlohmann::json& body
);
nlohmann::json require_current_project_payload(const holder::core::Paths& paths);
nlohmann::json card_api_request(
    const holder::core::Paths& paths,
    boost::beast::http::verb method,
    const std::string& target,
    const nlohmann::json& body = nlohmann::json::object(),
    boost::beast::http::status success = boost::beast::http::status::ok
);
nlohmann::json fetch_card_in_current_project(
    const holder::core::Paths& paths,
    const std::string& current_project_id,
    const std::string& card_id
);

} // namespace holder::cli
