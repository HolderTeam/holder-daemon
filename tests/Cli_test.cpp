#include "http_test_helpers.h"

#include "api/HttpServer.h"
#include "index/FtsIndexer.h"
#include "card/CardStore.h"
#include "store/Db.h"
#include "project/ProjectRepo.h"
#include "card/CardRepo.h"

#include <filesystem>
#include <fstream>
#include <sstream>
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
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (xdg_root / "keystore").string());

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
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (xdg_root / "keystore").string());

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
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR",
                                      (xdg_root / "keystore").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const std::string bin = HOLDER_BIN_PATH;
  const std::string cmd = "\"" + bin + "\" --reindex";
  const int code = run_command(cmd);
  REQUIRE(code == 0);

  const auto db_path = xdg_root / "data" / "holder" / "server" / "holder.db";
  REQUIRE(std::filesystem::exists(db_path));

  holder::store::Db db;
  db.open(db_path);
  holder::project::ProjectRepo project_repo(db);
  holder::card::CardRepo card_repo(db);

  const auto projects = project_repo.list();
  REQUIRE(projects.size() == 1);
  REQUIRE(projects[0].name == "Home");
  REQUIRE(projects[0].privacy_mode == "encrypted_git");
  REQUIRE(projects[0].project_key_id.has_value());
  REQUIRE_FALSE(projects[0].project_key_id->empty());

  const auto cards = card_repo.list_all(projects[0].project_id);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0].title == "Welcome to Holder");

  const auto welcome_path = repo_root / "config" / "WELCOME.md";
  std::ifstream welcome_file(welcome_path);
  REQUIRE(welcome_file.good());
  std::ostringstream expected;
  expected << welcome_file.rdbuf();

  const auto card_file_path = std::filesystem::path(projects[0].root_path) / cards[0].rel_path;
  REQUIRE(std::filesystem::exists(card_file_path));
  std::ifstream card_file_raw(card_file_path, std::ios::binary);
  REQUIRE(card_file_raw.good());
  std::ostringstream card_raw;
  card_raw << card_file_raw.rdbuf();
  const std::string raw_blob = card_raw.str();
  REQUIRE(raw_blob.rfind("HolderPriv1\n", 0) == 0);
  REQUIRE(raw_blob.find(expected.str()) == std::string::npos);

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  const auto content = card_store.get_content(cards[0]);
  REQUIRE(content.has_value());
  REQUIRE(content.value() == expected.str());

  // Second boot should not duplicate Home/welcome bootstrap content.
  const int second_code = run_command(cmd);
  REQUIRE(second_code == 0);

  holder::store::Db db2;
  db2.open(db_path);
  holder::project::ProjectRepo project_repo2(db2);
  holder::card::CardRepo card_repo2(db2);
  const auto projects2 = project_repo2.list();
  REQUIRE(projects2.size() == 1);
  const auto cards2 = card_repo2.list_all(projects2[0].project_id);
  REQUIRE(cards2.size() == 1);
}

TEST_CASE("Listener start fails on invalid bind address", "[listener]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::api::HttpServer server("invalid-bind", 0, db, "token", nullptr, nullptr);
  REQUIRE_THROWS(server.start());
}
