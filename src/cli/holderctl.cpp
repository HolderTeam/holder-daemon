#include "platform/Paths.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef CARD_SERVER_VERSION
#define CARD_SERVER_VERSION "0.0.0"
#endif

namespace {

struct ServerInfoFile {
  std::filesystem::path path;
  nlohmann::json json;
};

void print_usage(std::ostream& out) {
  out << "Usage: holderctl <command>\n"
      << "\n"
      << "Commands:\n"
      << "  token      Print the local daemon bearer token\n"
      << "  status     Print local daemon status\n"
      << "  health     Check daemon metadata, process state, token, and HTTP health\n"
      << "  paths      Print Holder data/config/cache paths\n"
      << "  projects   List Holder projects; use --json for raw API output\n"
      << "  openapi    Open local Swagger/OpenAPI docs; use --url to print the URL\n"
      << "  reindex    Rebuild the daemon search index from the local database\n"
      << "  restart    Restart the local daemon and rotate its bearer token\n"
      << "  logs       Print daemon logs; use --follow to tail or --path for the file path\n"
      << "  version    Print holderctl version\n";
}

bool is_process_running(int pid) {
  if (pid <= 0) return false;
#ifdef _WIN32
  return true;
#else
  if (::kill(pid, 0) == 0) return true;
  return errno == EPERM; // LCOV_EXCL_LINE: depends on host process ownership/permissions.
#endif
}

#ifndef _WIN32
void require_secure_file(const std::filesystem::path& path) {
  struct stat file_stat {};
  if (::lstat(path.c_str(), &file_stat) != 0) {
    throw std::runtime_error("Cannot inspect token file: " + path.string());
  }
  if (S_ISLNK(file_stat.st_mode)) {
    throw std::runtime_error("Refusing token file symlink: " + path.string());
  }
  if (!S_ISREG(file_stat.st_mode)) {
    throw std::runtime_error("Token file is not a regular file: " + path.string());
  }
  if (file_stat.st_uid != ::geteuid()) { // LCOV_EXCL_LINE: requires a file owned by another user.
    throw std::runtime_error("Token file is not owned by the current user: " + path.string()); // LCOV_EXCL_LINE
  }
  if ((file_stat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    throw std::runtime_error("Token file must be readable only by its owner: " + path.string());
  }

  const auto parent = path.parent_path();
  struct stat dir_stat {};
  if (::stat(parent.c_str(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) { // LCOV_EXCL_LINE: parent exists for a normal info path.
    throw std::runtime_error("Cannot inspect token directory: " + parent.string()); // LCOV_EXCL_LINE
  }
  if (dir_stat.st_uid != ::geteuid()) { // LCOV_EXCL_LINE: requires a directory owned by another user.
    throw std::runtime_error("Token directory is not owned by the current user: " + parent.string()); // LCOV_EXCL_LINE
  }
  if ((dir_stat.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    throw std::runtime_error("Token directory must be accessible only by its owner: " + parent.string());
  }
}
#else
void require_secure_file(const std::filesystem::path&) {}
#endif

ServerInfoFile read_server_info(const holder::core::Paths& paths) {
  const auto info_path = paths.info_path();
  std::ifstream in(info_path);
  if (!in.is_open()) {
    throw std::runtime_error("Holder daemon info file not found: " + info_path.string());
  }
  return {.path = info_path, .json = nlohmann::json::parse(in)};
}

std::string json_string(const nlohmann::json& json, const char* key, const std::string& fallback = "") {
  if (!json.contains(key) || json.at(key).is_null()) return fallback;
  return json.at(key).get<std::string>();
}

int json_int(const nlohmann::json& json, const char* key, int fallback = 0) {
  if (!json.contains(key) || json.at(key).is_null()) return fallback;
  return json.at(key).get<int>();
}

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

bool http_health_ok(const std::string& bind,
                    int port,
                    const std::string& token,
                    std::string* detail) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(bind, std::to_string(port));

    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(2));
    stream.connect(endpoints);

    http::request<http::empty_body> req{http::verb::get, "/health", 11};
    req.set(http::field::host, bind);
    req.set(http::field::user_agent, "holderctl");
    req.set(http::field::authorization, "Bearer " + token);
    req.keep_alive(false);

    http::write(stream, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    boost::system::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    if (res.result() != http::status::ok) {
      if (detail) *detail = "HTTP " + std::to_string(res.result_int());
      return false;
    }

    const auto body = nlohmann::json::parse(res.body());
    const bool ok = body.value("ok", false);
    if (detail) *detail = ok ? "ok" : "response ok=false";
    return ok;
  } catch (const std::exception& ex) {
    if (detail) *detail = ex.what();
    return false;
  }
}

int command_health(const holder::core::Paths& paths) {
  try {
    require_secure_file(paths.info_path());
    const auto info = read_server_info(paths);

    const int pid = json_int(info.json, "pid");
    const bool running = is_process_running(pid);
    const auto bind = json_string(info.json, "bind", "127.0.0.1");
    const int port = json_int(info.json, "port", 11499);
    const auto token = json_string(info.json, "auth_token");

    std::string http_detail = "not checked";
    const bool has_token = !token.empty();
    const bool http_ok = running && has_token && http_health_ok(bind, port, token, &http_detail);
    const bool healthy = running && has_token && http_ok;

    std::cout << "Holder daemon: " << (healthy ? "healthy" : "unhealthy") << "\n"
              << "PID: " << pid << (running ? " (running)" : " (not running)") << "\n"
              << "URL: http://" << bind << ":" << port << "\n"
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

std::string openapi_url(const holder::core::Paths& paths) {
  try {
    const auto info = read_server_info(paths);
    const auto bind = json_string(info.json, "bind", "127.0.0.1");
    const int port = json_int(info.json, "port", 11499);
    return "http://" + bind + ":" + std::to_string(port) + "/docs";
  } catch (const std::exception&) {
    return "http://127.0.0.1:11499/docs";
  }
}

int command_openapi(const holder::core::Paths& paths, int argc, char* argv[]) {
  bool url_only = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--url") {
      url_only = true;
    } else {
      throw std::runtime_error("Unknown openapi option: " + arg);
    }
  }

  const auto url = openapi_url(paths);
  if (url_only) {
    std::cout << url << "\n";
    return 0;
  }

#if defined(__linux__)
  const auto opener = boost::process::v2::environment::find_executable("xdg-open");
  if (opener.empty()) {
    std::cout << url << "\n";
    throw std::runtime_error("xdg-open not found");
  }

  boost::asio::io_context ioc;
  boost::process::v2::process proc(ioc.get_executor(), opener, {url});

  boost::system::error_code ec;
  const int exit_code = proc.wait(ec);
  if (ec) {
    std::cout << url << "\n";
    throw std::runtime_error("Failed to run xdg-open: " + ec.message());
  }
  if (exit_code != 0) {
    std::cout << url << "\n";
    throw std::runtime_error("xdg-open failed with exit code " + std::to_string(exit_code));
  }
  return 0;
#else
  std::cout << url << "\n";
  throw std::runtime_error("openapi browser launch is not supported on this platform yet"); // LCOV_EXCL_LINE
#endif
}

int command_restart() {
#if defined(__linux__)
  const auto systemctl = boost::process::v2::environment::find_executable("systemctl");
  if (systemctl.empty()) {
    throw std::runtime_error("systemctl not found; restart holder-daemon.service manually");
  }

  boost::asio::io_context ioc;
  boost::process::v2::process proc(
      ioc.get_executor(), systemctl, {"--user", "restart", "holder-daemon.service"});

  boost::system::error_code ec;
  const int exit_code = proc.wait(ec);
  if (ec) {
    throw std::runtime_error("Failed to run systemctl: " + ec.message());
  }
  if (exit_code != 0) {
    throw std::runtime_error("systemctl --user restart holder-daemon.service failed with exit code " +
                             std::to_string(exit_code));
  }

  std::cout << "Holder daemon restarted. A new local API token was generated.\n";
  return 0;
#else
  throw std::runtime_error("restart is not supported on this platform yet"); // LCOV_EXCL_LINE
#endif
}

int command_logs(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto log_path = paths.log_dir() / "server.log";
  bool follow = false;
  bool path_only = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--follow" || arg == "-f") {
      follow = true;
    } else if (arg == "--path") {
      path_only = true;
    } else {
      throw std::runtime_error("Unknown logs option: " + arg);
    }
  }

  if (path_only) {
    std::cout << log_path.string() << "\n";
    return 0;
  }

  if (follow) {
#if defined(__linux__)
    const auto tail = boost::process::v2::environment::find_executable("tail");
    if (tail.empty()) {
      throw std::runtime_error("tail not found; log file is: " + log_path.string());
    }

    boost::asio::io_context ioc;
    boost::process::v2::process proc(ioc.get_executor(), tail, {"-f", log_path.string()});

    boost::system::error_code ec;
    const int exit_code = proc.wait(ec);
    if (ec) {
      throw std::runtime_error("Failed to run tail: " + ec.message());
    }
    return exit_code;
#else
    throw std::runtime_error("logs --follow is not supported on this platform yet"); // LCOV_EXCL_LINE
#endif
  }

  std::ifstream in(log_path);
  if (!in.is_open()) {
    throw std::runtime_error("Holder daemon log file not found: " + log_path.string());
  }
  std::cout << in.rdbuf();
  return 0;
}

int command_reindex(const holder::core::Paths& paths, int argc) {
  if (argc > 2) {
    throw std::runtime_error("reindex does not take options");
  }

  require_secure_file(paths.info_path());
  const auto info = read_server_info(paths);
  const auto bind = json_string(info.json, "bind", "127.0.0.1");
  const int port = json_int(info.json, "port", 11499);
  const auto token = json_string(info.json, "auth_token");
  if (token.empty()) {
    throw std::runtime_error("Holder daemon info file has no auth_token: " + info.path.string());
  }

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(bind, std::to_string(port));

    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(30));
    stream.connect(endpoints);

    http::request<http::empty_body> req{http::verb::post, "/reindex", 11};
    req.set(http::field::host, bind);
    req.set(http::field::user_agent, "holderctl");
    req.set(http::field::authorization, "Bearer " + token);
    req.keep_alive(false);

    http::write(stream, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    boost::system::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    const auto payload = nlohmann::json::parse(res.body());
    if (res.result() != http::status::ok || !payload.value("ok", false)) {
      std::string message = "HTTP " + std::to_string(res.result_int());
      if (payload.contains("error") && payload["error"].contains("message")) {
        message = payload["error"]["message"].get<std::string>();
      }
      throw std::runtime_error("Reindex failed: " + message);
    }

    std::cout << payload.value("data", nlohmann::json::object())
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

  require_secure_file(paths.info_path());
  const auto info = read_server_info(paths);
  const auto bind = json_string(info.json, "bind", "127.0.0.1");
  const int port = json_int(info.json, "port", 11499);
  const auto token = json_string(info.json, "auth_token");
  if (token.empty()) {
    throw std::runtime_error("Holder daemon info file has no auth_token: " + info.path.string());
  }

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  try {
    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(bind, std::to_string(port));

    boost::beast::tcp_stream stream(ioc);
    stream.expires_after(std::chrono::seconds(10));
    stream.connect(endpoints);

    const std::string target = include_count ? "/projects?count=true" : "/projects";
    http::request<http::empty_body> req{http::verb::get, target, 11};
    req.set(http::field::host, bind);
    req.set(http::field::user_agent, "holderctl");
    req.set(http::field::authorization, "Bearer " + token);
    req.keep_alive(false);

    http::write(stream, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(stream, buffer, res);

    boost::system::error_code ec;
    stream.socket().shutdown(tcp::socket::shutdown_both, ec);

    const auto payload = nlohmann::json::parse(res.body());
    if (res.result() != http::status::ok || !payload.value("ok", false)) {
      std::string message = "HTTP " + std::to_string(res.result_int());
      if (payload.contains("error") && payload["error"].contains("message")) {
        message = payload["error"]["message"].get<std::string>();
      }
      throw std::runtime_error("Projects request failed: " + message);
    }

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

} // namespace

int main(int argc, char* argv[]) {
  try {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
      print_usage(std::cout);
      return 0;
    }

    const std::string command = argv[1];
    if (command == "--version" || command == "version") {
      std::cout << "holderctl " << CARD_SERVER_VERSION << "\n";
      return 0;
    }
    if (command == "restart") return command_restart();

    const auto paths = holder::core::Paths::resolve("holder");
    if (command == "token") return command_token(paths);
    if (command == "status") return command_status(paths);
    if (command == "health") return command_health(paths);
    if (command == "paths") return command_paths(paths);
    if (command == "projects") return command_projects(paths, argc, argv);
    if (command == "openapi") return command_openapi(paths, argc, argv);
    if (command == "logs") return command_logs(paths, argc, argv);
    if (command == "reindex") return command_reindex(paths, argc);

    std::cerr << "Unknown command: " << command << "\n";
    print_usage(std::cerr);
    return 2;
  } catch (const std::exception& ex) {
    std::cerr << "holderctl: " << ex.what() << "\n";
    return 1;
  }
}
