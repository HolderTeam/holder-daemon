#include "platform/Paths.h"

#include <nlohmann/json.hpp>

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
      << "  paths      Print Holder data/config/cache paths\n"
      << "  openapi    Print the local Swagger/OpenAPI docs URL\n"
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

int command_paths(const holder::core::Paths& paths) {
  std::cout << "Data:   " << paths.data_dir.string() << "\n";
  std::cout << "DB:     " << paths.db_path().string() << "\n";
  std::cout << "Info:   " << paths.info_path().string() << "\n";
  std::cout << "Logs:   " << (paths.log_dir() / "server.log").string() << "\n";
  std::cout << "Config: " << paths.config_dir.string() << "\n";
  std::cout << "Cache:  " << paths.cache_dir.string() << "\n";
  return 0;
}

int command_openapi(const holder::core::Paths& paths) {
  try {
    const auto info = read_server_info(paths);
    const auto bind = json_string(info.json, "bind", "127.0.0.1");
    const int port = json_int(info.json, "port", 11499);
    std::cout << "http://" << bind << ":" << port << "/docs\n";
  } catch (const std::exception&) {
    std::cout << "http://127.0.0.1:11499/docs\n";
  }
  return 0;
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

    const auto paths = holder::core::Paths::resolve("holder");
    if (command == "token") return command_token(paths);
    if (command == "status") return command_status(paths);
    if (command == "paths") return command_paths(paths);
    if (command == "openapi") return command_openapi(paths);

    std::cerr << "Unknown command: " << command << "\n";
    print_usage(std::cerr);
    return 2;
  } catch (const std::exception& ex) {
    std::cerr << "holderctl: " << ex.what() << "\n";
    return 1;
  }
}
