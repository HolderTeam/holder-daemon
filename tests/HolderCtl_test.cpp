#include "http_test_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

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

void write_server_info(const std::filesystem::path& path,
                       int pid = 12345,
                       int port = 11499,
                       const std::string& token = "deadbeef") {
  std::ofstream out(path);
  out << "{\n"
      << "  \"pid\": " << pid << ",\n"
      << "  \"bind\": \"127.0.0.1\",\n"
      << "  \"port\": " << port << ",\n"
      << "  \"api_version\": \"0.1\",\n"
      << "  \"server_version\": \"0.1.0\",\n"
      << "  \"auth_token\": \"" << token << "\"\n"
      << "}\n";
}

void write_server_info_without_token(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "{\n"
      << "  \"pid\": 12345,\n"
      << "  \"bind\": \"127.0.0.1\",\n"
      << "  \"port\": 11499,\n"
      << "  \"api_version\": \"0.1\",\n"
      << "  \"server_version\": \"0.1.0\"\n"
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

#ifndef _WIN32
void write_fake_systemctl(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "printf '%s\\n' \"$@\" > \"$HOLDERCTL_FAKE_SYSTEMCTL_ARGS\"\n"
      << "exit \"${HOLDERCTL_FAKE_SYSTEMCTL_EXIT:-0}\"\n";
  out.close();
  ::chmod(path.c_str(), S_IRWXU);
}

void write_fake_tail(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "printf '%s\\n' \"$@\" > \"$HOLDERCTL_FAKE_TAIL_ARGS\"\n"
      << "exit \"${HOLDERCTL_FAKE_TAIL_EXIT:-0}\"\n";
  out.close();
  ::chmod(path.c_str(), S_IRWXU);
}

void write_fake_xdg_open(const std::filesystem::path& path) {
  std::ofstream out(path);
  out << "#!/bin/sh\n"
      << "printf '%s\\n' \"$@\" > \"$HOLDERCTL_FAKE_XDG_OPEN_ARGS\"\n"
      << "exit \"${HOLDERCTL_FAKE_XDG_OPEN_EXIT:-0}\"\n";
  out.close();
  ::chmod(path.c_str(), S_IRWXU);
}
#endif

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

TEST_CASE("holderctl token reports missing server info file", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

#ifndef _WIN32
  const auto server_dir = xdg_root / "data" / "holder" / "server";
  ::chmod(server_dir.c_str(), S_IRWXU);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl token refuses symlink server info", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto target_path = server_dir / "target.json";
  const auto info_path = server_dir / "holder.json";
  write_server_info(target_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(target_path.c_str(), S_IRUSR | S_IWUSR);
  REQUIRE(::symlink(target_path.c_str(), info_path.c_str()) == 0);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl token refuses non-regular server info", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  std::filesystem::create_directory(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
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

TEST_CASE("holderctl token refuses loose token directory permissions", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU | S_IRGRP | S_IXGRP);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl token requires auth token field", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info_without_token(info_path);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" token >/dev/null 2>/dev/null";
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
  REQUIRE(run_command(bin + " openapi --url") == 0);
  REQUIRE(run_command(bin + " logs --path") == 0);
  REQUIRE(run_command(bin + " --version") == 0);
  REQUIRE(run_command(bin + " --help") == 0);
  REQUIRE(run_command(bin + " nope") == 2);
}

TEST_CASE("holderctl status reports missing or stopped daemon", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " status") == 1);
  REQUIRE(run_command(bin + " openapi --url") == 0);

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, -1);
  REQUIRE(run_command(bin + " status") == 1);
}

TEST_CASE("holderctl health checks metadata process token and HTTP endpoint", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto db_path = xdg_root / "holder.db";
  holder::platform::Db db;
  db.open(db_path);

  const std::string token = "healthtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });
  REQUIRE(holder::test::wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), static_cast<int>(bound.port), token);
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR);
#endif

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" health";
  REQUIRE(run_command(cmd) == 0);

  server.stop();
  server_thread.join();
}

TEST_CASE("holderctl health reports missing or insecure metadata", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " health >/dev/null 2>/dev/null") == 1);

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()));
#ifndef _WIN32
  ::chmod(server_dir.c_str(), S_IRWXU);
  ::chmod(info_path.c_str(), S_IRUSR | S_IWUSR | S_IRGRP);
#endif
  REQUIRE(run_command(bin + " health >/dev/null 2>/dev/null") == 1);
}

TEST_CASE("holderctl logs prints daemon log file", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto log_dir = xdg_root / "data" / "holder" / "server" / "logs";
  std::filesystem::create_directories(log_dir);
  const auto log_path = log_dir / "server.log";
  {
    std::ofstream out(log_path);
    out << "one\n"
        << "two\n";
  }

  const auto out_path = xdg_root / "logs.out";
  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" logs > \"" +
                          out_path.string() + "\"";
  REQUIRE(run_command(cmd) == 0);
  REQUIRE(read_text(out_path) == "one\ntwo\n");
}

TEST_CASE("holderctl logs reports missing daemon log file", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" logs >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

TEST_CASE("holderctl logs rejects unknown options", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string cmd = std::string("\"") + HOLDER_CTL_PATH + "\" logs --bad >/dev/null 2>/dev/null";
  REQUIRE(run_command(cmd) == 1);
}

#ifndef _WIN32
TEST_CASE("holderctl openapi opens Swagger docs with xdg-open", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto server_dir = xdg_root / "data" / "holder" / "server";
  const auto info_path = server_dir / "holder.json";
  write_server_info(info_path, static_cast<int>(::getpid()), 12345);

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_xdg_open(fake_bin / "xdg-open");

  const auto args_path = xdg_root / "xdg-open.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH", fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{}));
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_XDG_OPEN_ARGS", args_path.string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " openapi") == 0);
  REQUIRE(read_text(args_path) == "http://127.0.0.1:12345/docs\n");
}

TEST_CASE("holderctl openapi reports xdg-open failure", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_xdg_open(fake_bin / "xdg-open");

  const auto args_path = xdg_root / "xdg-open.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH", fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{}));
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_XDG_OPEN_ARGS", args_path.string());
  holder::test::EnvGuard exit_env("HOLDERCTL_FAKE_XDG_OPEN_EXIT", "9");

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " openapi >/dev/null 2>/dev/null") == 1);
  REQUIRE(read_text(args_path) == "http://127.0.0.1:11499/docs\n");
}

TEST_CASE("holderctl openapi rejects unknown options", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " openapi --bad >/dev/null 2>/dev/null") == 1);
}

TEST_CASE("holderctl restart invokes the Linux user service", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_systemctl(fake_bin / "systemctl");

  const auto args_path = xdg_root / "systemctl.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH", fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{}));
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_SYSTEMCTL_ARGS", args_path.string());

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " restart") == 0);
  REQUIRE(read_text(args_path) == "--user\nrestart\nholder-daemon.service\n");
}

TEST_CASE("holderctl restart reports service manager failure", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_systemctl(fake_bin / "systemctl");

  const auto args_path = xdg_root / "systemctl.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH", fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{}));
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_SYSTEMCTL_ARGS", args_path.string());
  holder::test::EnvGuard exit_env("HOLDERCTL_FAKE_SYSTEMCTL_EXIT", "23");

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " restart >/dev/null 2>/dev/null") == 1);
  REQUIRE(read_text(args_path) == "--user\nrestart\nholder-daemon.service\n");
}

TEST_CASE("holderctl logs follow invokes tail", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_tail(fake_bin / "tail");

  const auto args_path = xdg_root / "tail.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH", fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{}));
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_TAIL_ARGS", args_path.string());

  const auto expected_log_path = xdg_root / "data" / "holder" / "server" / "logs" / "server.log";
  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " logs --follow") == 0);
  REQUIRE(read_text(args_path) == "-f\n" + expected_log_path.string() + "\n");
}

TEST_CASE("holderctl logs follow reports tail failure", "[holderctl]") {
  const auto xdg_root = prepare_xdg_tree();
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto fake_bin = xdg_root / "bin";
  std::filesystem::create_directories(fake_bin);
  write_fake_tail(fake_bin / "tail");

  const auto args_path = xdg_root / "tail.args";
  const char* old_path = std::getenv("PATH");
  holder::test::EnvGuard path_env(
      "PATH", fake_bin.string() + ":" + (old_path ? std::string(old_path) : std::string{}));
  holder::test::EnvGuard args_env("HOLDERCTL_FAKE_TAIL_ARGS", args_path.string());
  holder::test::EnvGuard exit_env("HOLDERCTL_FAKE_TAIL_EXIT", "11");

  const std::string bin = std::string("\"") + HOLDER_CTL_PATH + "\"";
  REQUIRE(run_command(bin + " logs --follow >/dev/null 2>/dev/null") == 11);
}
#endif
