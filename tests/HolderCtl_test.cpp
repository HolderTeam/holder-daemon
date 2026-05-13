#include "http_test_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

int run_command(const std::string& cmd) {
  const int rc = std::system(cmd.c_str());
#ifdef _WIN32
  return rc;
#else
  if (rc == -1) return rc;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return -1;
#endif
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

void write_server_info(const std::filesystem::path& path, int pid = 12345) {
  std::ofstream out(path);
  out << "{\n"
      << "  \"pid\": " << pid << ",\n"
      << "  \"bind\": \"127.0.0.1\",\n"
      << "  \"port\": 11499,\n"
      << "  \"api_version\": \"0.1\",\n"
      << "  \"server_version\": \"0.1.0\",\n"
      << "  \"auth_token\": \"deadbeef\"\n"
      << "}\n";
}

std::filesystem::path prepare_xdg_tree() {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root / "data" / "holder" / "server");
  std::filesystem::create_directories(xdg_root / "config");
  std::filesystem::create_directories(xdg_root / "cache");
  return xdg_root;
}

} // namespace

TEST_CASE("holderctl token prints token from secure server info", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const auto out_path = xdg_root / "token.out";
  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token > \"" + out_path.string() + "\"";
  REQUIRE(run_command(cmd) == 0);
  REQUIRE(read_text(out_path) == "deadbeef\n");
}

TEST_CASE("holderctl token refuses loose token file permissions", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl status paths openapi and version smoke", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()));
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " status") == 0);
  REQUIRE(run_command(bin + " paths") == 0);
  REQUIRE(run_command(bin + " openapi") == 0);
  REQUIRE(run_command(bin + " --version") == 0);
  REQUIRE(run_command(bin + " --help") == 0);
  REQUIRE(run_command(bin + " nope") == 2);
}
