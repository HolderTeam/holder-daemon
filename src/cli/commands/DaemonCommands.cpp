#include "cli/commands/Commands.h"

#include "cli/commands/Common.h"

#include <boost/beast/http.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace holder::cli {

int command_token(const holder::core::Paths& paths) {
  require_secure_file(paths.info_path());
  const auto info = read_server_info(paths);
  const auto token = json_string(info.json, "auth_token");
  if (token.empty()) {
    throw std::runtime_error("Holder daemon info file has no auth_token: " + info.path.string());
  }
  std::cout << token << "\n";
  return 0;
}

int command_status(const holder::core::Paths& paths) {
  try {
    const auto info = read_server_info(paths);
    const int pid = json_int(info.json, "pid");
    const bool running = is_process_running(pid);
    const auto bind = json_string(info.json, "bind", "127.0.0.1");
    const int port = json_int(info.json, "port", 11499);

    std::cout << "Holder daemon: " << (running ? "running" : "not running") << "\n"
              << "PID: " << pid << "\n"
              << "URL: http://" << bind << ":" << port << "\n"
              << "API version: " << json_string(info.json, "api_version", "unknown") << "\n"
              << "Server version: " << json_string(info.json, "server_version", "unknown") << "\n"
              << "Info file: " << info.path.string() << "\n";
    return running ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cout << "Holder daemon: not running\n"
              << "Info file: " << paths.info_path().string() << "\n"
              << "Reason: " << ex.what() << "\n";
    return 1;
  }
}

bool http_health_ok(const DaemonConnection& connection, std::string* detail) {
  try {
    const auto response = http_json_request(
        connection,
        boost::beast::http::verb::get,
        "/health",
        std::chrono::seconds(2)
    );
    if (response.status != boost::beast::http::status::ok) {
      if (detail) *detail = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
      return false;
    }

    const bool ok = response.payload.value("ok", false);
    if (detail) *detail = ok ? "ok" : "response ok=false";
    return ok;
  } catch (const std::exception& ex) {
    if (detail) *detail = ex.what();
    return false;
  }
}

int command_health(const holder::core::Paths& paths) {
  try {
    auto connection = read_secure_daemon_connection(paths);
    const auto& info = connection.info;

    const int pid = json_int(info.json, "pid");
    const bool running = is_process_running(pid);

    std::string http_detail = "not checked";
    const bool has_token = !connection.token.empty();
    const bool http_ok = running && has_token && http_health_ok(connection, &http_detail);
    const bool healthy = running && has_token && http_ok;

    std::cout << "Holder daemon: " << (healthy ? "healthy" : "unhealthy") << "\n"
              << "PID: " << pid << (running ? " (running)" : " (not running)") << "\n"
              << "URL: http://" << connection.bind << ":" << connection.port << "\n"
              << "API version: " << json_string(info.json, "api_version", "unknown") << "\n"
              << "Server version: " << json_string(info.json, "server_version", "unknown") << "\n"
              << "Info file: " << info.path.string() << "\n"
              << "Token file: " << (has_token ? "secure" : "missing auth_token") << "\n"
              << "HTTP health: " << http_detail << "\n";
    return healthy ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cout << "Holder daemon: unhealthy\n"
              << "Info file: " << paths.info_path().string() << "\n"
              << "Reason: " << ex.what() << "\n";
    return 1;
  }
}

int command_paths(const holder::core::Paths& paths) {
  std::cout << "Data:   " << paths.data_dir.string() << "\n";
  std::cout << "DB:     " << paths.db_path().string() << "\n";
  std::cout << "Info:   " << paths.info_path().string() << "\n";
  std::cout << "Logs:   " << (paths.log_dir() / "server.log").string() << "\n";
  std::cout << "Config: " << paths.config_dir.string() << "\n";
  std::cout << "Cache:  " << paths.cache_dir.string() << "\n";
  return 0;
}

} // namespace holder::cli
