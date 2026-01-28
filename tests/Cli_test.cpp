#include "http_test_helpers.h"

#include "api/HttpServer.h"

#include <filesystem>
#include <fstream>
#include <string>

#ifndef _WIN32
#include <sys/wait.h>
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

class CwdGuard {
 public:
  explicit CwdGuard(std::filesystem::path next) : prev_(std::filesystem::current_path()) {
    std::filesystem::current_path(std::move(next));
  }
  ~CwdGuard() { std::filesystem::current_path(prev_); }

 private:
  std::filesystem::path prev_;
};

} // namespace

TEST_CASE("CLI --port rejects invalid values", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const std::string bin = HOLDER_BIN_PATH;
  const std::string cmd = "\"" + bin + "\" --port 70000 --reindex";
  const int code = run_command(cmd);
  REQUIRE(code == 2);
}

TEST_CASE("CLI --port rejects non-numeric values", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const std::string bin = HOLDER_BIN_PATH;
  const std::string cmd = "\"" + bin + "\" --port nope --reindex";
  const int code = run_command(cmd);
  REQUIRE(code == 2);
}

TEST_CASE("CLI --reindex runs with temp XDG dirs", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const std::string bin = HOLDER_BIN_PATH;
  const std::string cmd = "\"" + bin + "\" --reindex";
  const int code = run_command(cmd);
  REQUIRE(code == 0);
}

TEST_CASE("Listener start fails on invalid bind address", "[listener]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::api::HttpServer server("invalid-bind", 0, db, "token", nullptr, nullptr);
  REQUIRE_THROWS(server.start());
}
