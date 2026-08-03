#include "cli/commands/Commands.h"

#include "cli/commands/Common.h"
#include "cli/commands/Support.h"

#include <boost/asio.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace holder::cli {

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

  open_external_uri(url);
  return 0;
}

int command_restart() {
#if defined(__linux__)
  const auto systemctl = boost::process::v2::environment::find_executable("systemctl");
  if (systemctl.empty()) {
    throw std::runtime_error("systemctl not found; restart holder-daemon.service manually"
    ); // LCOV_EXCL_LINE: depends on host PATH contents.
  }

  boost::asio::io_context ioc;
  boost::process::v2::process proc(
      ioc.get_executor(),
      systemctl,
      {"--user", "restart", "holder-daemon.service"}
  );

  boost::system::error_code ec;
  const int exit_code = proc.wait(ec);
  if (ec) {
    // LCOV_EXCL_START
    throw std::runtime_error("Failed to run systemctl: " + ec.message());
    // LCOV_EXCL_STOP
  }
  if (exit_code != 0) {
    throw std::runtime_error(
        "systemctl --user restart holder-daemon.service failed with exit code " +
        std::to_string(exit_code)
    );
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
#if !defined(_WIN32)
    const auto tail = boost::process::v2::environment::find_executable("tail");
    if (tail.empty()) {
      // LCOV_EXCL_START
      throw std::runtime_error("tail not found; log file is: " + log_path.string());
      // LCOV_EXCL_STOP
    }

    boost::asio::io_context ioc;
    boost::process::v2::process proc(ioc.get_executor(), tail, {"-f", log_path.string()});

    boost::system::error_code ec;
    const int exit_code = proc.wait(ec);
    if (ec) {
      // LCOV_EXCL_START
      throw std::runtime_error("Failed to run tail: " + ec.message());
      // LCOV_EXCL_STOP
    }
    return exit_code;
#else
    throw std::runtime_error("logs --follow is not supported on this platform yet"
    ); // LCOV_EXCL_LINE
#endif
  }

  std::ifstream in(log_path);
  if (!in.is_open()) {
    throw std::runtime_error("Holder daemon log file not found: " + log_path.string());
  }
  std::cout << in.rdbuf();
  return 0;
}

} // namespace holder::cli
