#pragma once

#include "platform/Paths.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>

namespace holder::cli {

struct ServerInfoFile {
  std::filesystem::path path;
  nlohmann::json json;
};

struct DaemonConnection {
  ServerInfoFile info;
  std::string bind;
  int port = 0;
  std::string token;
};

struct HttpJsonResponse {
  boost::beast::http::status status;
  nlohmann::json payload;
};

void print_usage(std::ostream& out);
bool is_process_running(int pid);
void require_secure_file(const std::filesystem::path& path);
ServerInfoFile read_server_info(const holder::core::Paths& paths);
std::string json_string(
    const nlohmann::json& json,
    const char* key,
    const std::string& fallback = ""
);
int json_int(const nlohmann::json& json, const char* key, int fallback = 0);
DaemonConnection read_secure_daemon_connection(const holder::core::Paths& paths);
HttpJsonResponse http_json_request(
    const DaemonConnection& connection,
    boost::beast::http::verb method,
    const std::string& target,
    std::chrono::seconds timeout,
    const std::optional<nlohmann::json>& body = std::nullopt
);

} // namespace holder::cli
