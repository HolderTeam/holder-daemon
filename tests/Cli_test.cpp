#include "TestCommand.h"
#include "http_test_helpers.h"

#include "api/HttpServer.h"
#include "card/CardRepo.h"
#include "card/CardStore.h"
#include "card/TagRepo.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "platform/Db.h"
#include "platform/LockFile.h"
#include "platform/Migrations.h"
#include "platform/Paths.h"
#include "platform/ProjectRegistry.h"
#include "project/ProjectManifest.h"
#include "project/ProjectRepo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

namespace {

int run_command(const std::string& cmd) { return holder::test::run_system_command(cmd); }

class CwdGuard {
 public:
  explicit CwdGuard(const std::filesystem::path& next)
      : prev_(std::filesystem::current_path()) {
    std::filesystem::current_path(next);
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
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

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
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

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
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const std::string bin = HOLDER_BIN_PATH;
  const std::string cmd = "\"" + bin + "\" --reindex";
  const int code = run_command(cmd);
  REQUIRE(code == 0);

  const auto db_path = xdg_root / "data" / "holder" / "server" / "holder.db";
  REQUIRE(std::filesystem::exists(db_path));

  holder::platform::Db db;
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

  holder::platform::Db db2;
  db2.open(db_path);
  holder::project::ProjectRepo project_repo2(db2);
  holder::card::CardRepo card_repo2(db2);
  const auto projects2 = project_repo2.list();
  REQUIRE(projects2.size() == 1);
  const auto cards2 = card_repo2.list_all(projects2[0].project_id);
  REQUIRE(cards2.size() == 1);
}

TEST_CASE(
    "holderctl database rebuild dry-run invokes the offline daemon command",
    "[cli][database]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());
  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  REQUIRE(run_command("\"" + std::string(HOLDER_CTL_PATH) + "\" database rebuild --dry-run") == 0);
  REQUIRE_FALSE(std::filesystem::exists(xdg_root / "data" / "holder" / "server" / "holder.db"));
}

TEST_CASE("CLI upgrades a v1 database and backfills card tags", "[cli][migrations]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);
  const std::string cmd = "\"" + std::string(HOLDER_BIN_PATH) + "\" --reindex";
  REQUIRE(run_command(cmd) == 0);

  const auto db_path = xdg_root / "data" / "holder" / "server" / "holder.db";
  std::string project_id;
  std::string card_id;
  std::filesystem::path project_root;
  {
    holder::platform::Db db;
    db.open(db_path);
    holder::project::ProjectRepo project_repo(db);
    holder::card::CardRepo card_repo(db);
    const auto projects = project_repo.list();
    REQUIRE(projects.size() == 1);
    project_id = projects[0].project_id;
    project_root = projects[0].root_path;
    const auto cards = card_repo.list_all(project_id);
    REQUIRE(cards.size() == 1);
    card_id = cards[0].card_id;

    holder::index::FtsIndexer fts(db);
    holder::card::CardStore card_store(db, &fts);
    card_store.update_content(card_id, "Migration content #legacy-tag", std::nullopt, 100);
    holder::model::Card missing;
    missing.card_id = "missing-content-card";
    missing.project_id = project_id;
    missing.title = "Missing content";
    missing.created_at = 101;
    missing.updated_at = 101;
    card_store.create(missing, "temporary");
    const auto missing_row = card_store.get(missing.card_id);
    REQUIRE(missing_row.has_value());
    std::filesystem::remove(project_root / missing_row->rel_path);
    db.exec(
        "INSERT INTO cards(card_id,project_id,title,rel_path,sort_key,created_at,updated_at) "
        "VALUES('invalid-path-card','" +
        project_id + "','Invalid path','../outside.md',999,102,102);"
    );
    db.exec("DROP TABLE card_tags;");
    db.exec("UPDATE schema_version SET version = 1;");
  }

  std::filesystem::remove(project_root / holder::project::kProjectManifestPath);
  REQUIRE_FALSE(holder::project::has_project_manifest(project_root));

  REQUIRE(run_command(cmd) == 0);

  holder::platform::Db upgraded_db;
  upgraded_db.open(db_path);
  REQUIRE_NOTHROW(holder::platform::Migrations::ensure_schema_version(
      upgraded_db,
      holder::platform::Migrations::latest_schema_version
  ));
  holder::card::TagRepo tag_repo(upgraded_db);
  REQUIRE(
      tag_repo.list_tags_for_card(project_id, card_id) == std::vector<std::string>{"legacy-tag"}
  );
  REQUIRE(holder::project::has_project_manifest(project_root));
}

TEST_CASE("CLI startup recovers projects from the durable registry", "[cli][recovery]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());
  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  auto paths = holder::core::Paths::resolve("holder");
  paths.ensure_dirs();
  {
    auto db = holder::test::open_db_with_schema(paths.db_path());
    db.close();
  }
  holder::model::Project project;
  project.project_id = "registry-project";
  project.name = "Registry project";
  project.root_path = (dir / "external-project").string();
  project.privacy_mode = "plain";
  project.created_at = 1;
  project.updated_at = 1;
  holder::git::RealGitOps git;
  git.open_or_init(project.root_path);
  holder::project::write_project_manifest(git, project);
  git.commit("Create durable project metadata");
  holder::core::ProjectRegistry(paths.project_registry_path()).remember({project});

  REQUIRE(run_command("\"" + std::string(HOLDER_BIN_PATH) + "\" --reindex") == 0);
  holder::platform::Db recovered;
  recovered.open(paths.db_path());
  REQUIRE(holder::project::ProjectRepo(recovered).get(project.project_id).has_value());
}

TEST_CASE("CLI --help and unknown args branches", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const std::string bin = HOLDER_BIN_PATH;
  REQUIRE(run_command("\"" + bin + "\" --help") == 0);
  REQUIRE(run_command("\"" + bin + "\" --version") == 0);
  REQUIRE(run_command("\"" + bin + "\" --wat") == 2);
}

TEST_CASE("CLI --bind and valid --port parse paths", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const std::string bin = HOLDER_BIN_PATH;
  const std::string cmd = "\"" + bin + "\" --bind 127.0.0.1 --port 12345 --reindex";
  REQUIRE(run_command(cmd) == 0);
}

TEST_CASE("CLI reindex resolves schema and welcome from parent of build cwd", "[cli]") {
#ifdef _WIN32
  SKIP("Visual Studio out/build layout is not a source-tree child build directory");
#else
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const std::string bin = HOLDER_BIN_PATH;
  const auto build_dir = std::filesystem::path(bin).parent_path();
  REQUIRE(std::filesystem::exists(build_dir));
  CwdGuard cwd(build_dir);

  REQUIRE(run_command("\"" + bin + "\" --reindex") == 0);
#endif
}

TEST_CASE("CLI reindex fails when schema cannot be found", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto isolated = dir / "isolated";
  std::filesystem::create_directories(isolated);
  CwdGuard cwd(isolated);

  const std::string bin = HOLDER_BIN_PATH;
  REQUIRE(run_command("\"" + bin + "\" --reindex") != 0);
}

TEST_CASE("CLI reindex fails when welcome markdown path exists but cannot be opened", "[cli]") {
#ifdef _WIN32
  SKIP("POSIX permission-bit unreadable-file test is not meaningful on Windows");
#else
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto isolated = dir / "with_schema";
  std::filesystem::create_directories(isolated / "schema");
  std::filesystem::create_directories(isolated / "config");
  std::filesystem::copy_file(
      std::filesystem::path(SCHEMA_SQL_PATH),
      isolated / "schema" / "schema.sql"
  );
  const auto welcome = isolated / "config" / "WELCOME.md";
  {
    std::ofstream out(welcome, std::ios::trunc);
    out << "# Welcome\n";
  }
  std::filesystem::permissions(
      welcome,
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
          std::filesystem::perms::group_read | std::filesystem::perms::others_read,
      std::filesystem::perm_options::remove
  );
  CwdGuard cwd(isolated);

  const std::string bin = HOLDER_BIN_PATH;
  REQUIRE(run_command("\"" + bin + "\" --reindex") != 0);
#endif
}

TEST_CASE("CLI reindex fails when schema exists but welcome markdown is missing", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto isolated = dir / "with_schema_no_welcome";
  std::filesystem::create_directories(isolated / "schema");
  std::filesystem::copy_file(
      std::filesystem::path(SCHEMA_SQL_PATH),
      isolated / "schema" / "schema.sql"
  );
  CwdGuard cwd(isolated);

  const std::string bin = HOLDER_BIN_PATH;
  REQUIRE(run_command("\"" + bin + "\" --reindex") != 0);
}

TEST_CASE("CLI welcome title falls back when first markdown line is not a heading", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto isolated = dir / "with_custom_welcome";
  std::filesystem::create_directories(isolated / "schema");
  std::filesystem::create_directories(isolated / "config");
  std::filesystem::copy_file(
      std::filesystem::path(SCHEMA_SQL_PATH),
      isolated / "schema" / "schema.sql"
  );
  {
    std::ofstream out(isolated / "config" / "WELCOME.md", std::ios::trunc);
    out << "Not a heading first line\nBut still content\n";
  }
  CwdGuard cwd(isolated);

  const std::string bin = HOLDER_BIN_PATH;
  REQUIRE(run_command("\"" + bin + "\" --reindex") == 0);

  holder::platform::Db db;
  db.open(xdg_root / "data" / "holder" / "server" / "holder.db");
  holder::project::ProjectRepo project_repo(db);
  holder::card::CardRepo card_repo(db);
  const auto projects = project_repo.list();
  REQUIRE(projects.size() == 1);
  const auto cards = card_repo.list_all(projects[0].project_id);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0].title == "Welcome");
}

TEST_CASE("CLI welcome title falls back when first markdown line is blank", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto isolated = dir / "with_blank_first_line";
  std::filesystem::create_directories(isolated / "schema");
  std::filesystem::create_directories(isolated / "config");
  std::filesystem::copy_file(
      std::filesystem::path(SCHEMA_SQL_PATH),
      isolated / "schema" / "schema.sql"
  );
  {
    std::ofstream out(isolated / "config" / "WELCOME.md", std::ios::trunc);
    out << "   \n# Heading on second line\n";
  }
  CwdGuard cwd(isolated);

  const std::string bin = HOLDER_BIN_PATH;
  REQUIRE(run_command("\"" + bin + "\" --reindex") == 0);

  holder::platform::Db db;
  db.open(xdg_root / "data" / "holder" / "server" / "holder.db");
  holder::project::ProjectRepo project_repo(db);
  holder::card::CardRepo card_repo(db);
  const auto projects = project_repo.list();
  REQUIRE(projects.size() == 1);
  const auto cards = card_repo.list_all(projects[0].project_id);
  REQUIRE(cards.size() == 1);
  REQUIRE(cards[0].title == "Welcome");
}

TEST_CASE("CLI exits when lock file is busy", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  CwdGuard cwd(repo_root);

  const auto paths = holder::core::Paths::resolve("holder");
  paths.ensure_dirs();
  holder::core::LockFile lock(paths.lock_path());
  REQUIRE(lock.try_acquire());

  const std::string bin = HOLDER_BIN_PATH;
  REQUIRE(run_command("\"" + bin + "\" --reindex") == 2);
}

#ifndef _WIN32
TEST_CASE("CLI normal run starts server and exits on SIGTERM", "[cli]") {
  const auto dir = holder::test::make_temp_dir();
  const auto xdg_root = dir / "xdg";
  std::filesystem::create_directories(xdg_root);

  holder::test::EnvGuard data_env("XDG_DATA_HOME", (xdg_root / "data").string());
  holder::test::EnvGuard config_env("XDG_CONFIG_HOME", (xdg_root / "config").string());
  holder::test::EnvGuard cache_env("XDG_CACHE_HOME", (xdg_root / "cache").string());
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", (xdg_root / "keystore").string());

  const auto repo_root = std::filesystem::path(__FILE__).parent_path().parent_path();
  const std::string bin = HOLDER_BIN_PATH;

  pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    std::filesystem::current_path(repo_root);
    execl(bin.c_str(), bin.c_str(), "--bind", "127.0.0.1", "--port", "0", nullptr);
    _exit(127);
  }

  const auto info_path = xdg_root / "data" / "holder" / "server" / "holder.json";
  bool info_written = false;
  for (int i = 0; i < 100; ++i) {
    if (std::filesystem::exists(info_path)) {
      info_written = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(info_written);

  REQUIRE(kill(pid, SIGTERM) == 0);
  int status = 0;
  REQUIRE(waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFEXITED(status));
  REQUIRE(WEXITSTATUS(status) == 0);

  std::ifstream in(info_path);
  REQUIRE(in.good());
  const auto json = nlohmann::json::parse(in);
  REQUIRE(json.contains("bind"));
  REQUIRE(json.contains("port"));
  REQUIRE(json.contains("pid"));
}
#endif

TEST_CASE("Listener start fails on invalid bind address", "[listener]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::api::HttpServer server("invalid-bind", 0, db, "token", nullptr, nullptr);
  REQUIRE_THROWS(server.start());
}

#ifndef _WIN32
TEST_CASE(
    "CLI health monitor stops when its database becomes corrupt or disappears",
    "[cli][recovery]"
) {
  const auto root = holder::test::make_temp_dir();
  holder::test::EnvGuard data("XDG_DATA_HOME", (root / "data").string());
  holder::test::EnvGuard config("XDG_CONFIG_HOME", (root / "config").string());
  holder::test::EnvGuard cache("XDG_CACHE_HOME", (root / "cache").string());
  holder::test::EnvGuard keys("HOLDER_TEST_KEYSTORE_DIR", (root / "keys").string());
  CwdGuard cwd(std::filesystem::path(__FILE__).parent_path().parent_path());
  bool corrupt = false;
  SECTION("corruption after a healthy check") { corrupt = true; }
  SECTION("missing database") {}
  const std::string binary = HOLDER_BIN_PATH;
  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    execl(binary.c_str(), binary.c_str(), "--bind", "127.0.0.1", "--port", "0", nullptr);
    _exit(127);
  }
  struct ChildGuard {
    pid_t pid;
    bool reaped = false;
    ~ChildGuard() {
      if (!reaped) {
        kill(pid, SIGKILL);
        int status = 0;
        waitpid(pid, &status, 0);
      }
    }
  } child{pid};
  const auto database = root / "data/holder/server/holder.db";
  const auto info = root / "data/holder/server/holder.json";
  const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!std::filesystem::exists(info) && std::chrono::steady_clock::now() < startup_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  REQUIRE(std::filesystem::exists(info));
  if (corrupt) std::this_thread::sleep_for(std::chrono::seconds(6));
  // Keep the live connection's inode intact; the monitor checks the public path.
  std::filesystem::rename(database, root / "original.db");
  if (corrupt) std::ofstream(database) << "not a SQLite database";
  int status = 0;
  const auto shutdown_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < shutdown_deadline) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      child.reaped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  REQUIRE(child.reaped);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) != 0);
}
#endif

TEST_CASE("CLI rejects dry-run without rebuilding", "[cli][recovery]") {
  const auto root = holder::test::make_temp_dir();
  holder::test::EnvGuard data("XDG_DATA_HOME", (root / "data").string());
  holder::test::EnvGuard config("XDG_CONFIG_HOME", (root / "config").string());
  holder::test::EnvGuard cache("XDG_CACHE_HOME", (root / "cache").string());
  CwdGuard cwd(std::filesystem::path(__FILE__).parent_path().parent_path());
  CHECK(run_command("\"" + std::string(HOLDER_BIN_PATH) + "\" --dry-run") == 2);
}
