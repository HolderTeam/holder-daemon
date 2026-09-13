#pragma once

#include "platform/Paths.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <stdexcept>
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

class CliError : public std::runtime_error {
 public:
  CliError(
      std::string code,
      std::string message,
      nlohmann::json details = nlohmann::json::object(),
      std::string human_message = "",
      int exit_code = 1
  );

  const std::string& code() const noexcept;
  const std::string& message() const noexcept;
  const nlohmann::json& details() const noexcept;
  int exit_code() const noexcept;

 private:
  std::string code_;
  std::string message_;
  nlohmann::json details_;
  int exit_code_;
};

void print_usage(std::ostream& out);
bool json_output_requested(int argc, char* argv[]);
void print_cli_error(std::ostream& out, const std::exception& error, bool json_output);
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
