#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "model/Project.h"
#include "index/FtsIndexer.h"
#include "ai/AiMessagePaths.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "git/GitOps.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <vector>

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path p = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(p)) return p;
#endif
  namespace fs = std::filesystem;
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_ai_message_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(holder::platform::Db& db,
                    const std::string& project_id,
                    const std::string& root_path) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path;
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

void update_project_remote(holder::platform::Db& db,
                           const std::string& project_id,
                           const std::string& remote_url,
                           long long updated_at) {
  holder::project::ProjectRepo repo(db);
  repo.update_git_remote(project_id, std::optional<std::string>(remote_url), updated_at);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void create_thread(holder::platform::Db& db, const std::string& thread_id, const std::string& project_id) {
  holder::ai::AiThreadRepo repo(db);
  holder::model::AiThread thread;
  thread.thread_id = thread_id;
  thread.project_id = project_id;
  thread.title = "Thread";
  thread.created_at = 2;
  thread.updated_at = 2;
  repo.create(thread);
}

class TrackingGitOps final : public holder::git::GitOps {
public:
  std::filesystem::path repo_root;
  int set_remote_calls = 0;
  std::vector<std::filesystem::path> removed_paths;

  void open_or_init(const std::filesystem::path& repo_dir) override {
    repo_root = repo_dir;
    std::filesystem::create_directories(repo_root);
  }
  void write_file(const std::filesystem::path& relative_path,
                  const std::string& content) override {
    const auto full_path = repo_root / relative_path;
    std::filesystem::create_directories(full_path.parent_path());
    std::ofstream out(full_path, std::ios::binary);
    if (!out.is_open()) {
      throw std::runtime_error("Failed to write file");
    }
    out << content;
  }
  void stage_path(const std::filesystem::path&) override {}
  void remove_path(const std::filesystem::path& relative_path) override {
    removed_paths.push_back(relative_path);
  }
  void commit(const std::string&) override {}
  void set_remote(const std::string&, const std::string&) override {
    set_remote_calls++;
  }
  void remove_remote(const std::string&) override {}
  void pull_remote_ff_only(const std::string&) override {}
  holder::git::RemoteProbeResult probe_remote(const std::string&) override {
    return {.status = holder::git::RemoteProbeStatus::Reachable, .remote_has_head = true, .error_message = {}};
  }
  holder::git::PushResult push_branch(const std::string&,
                                      const std::string&,
                                      bool) override {
    return {.status = holder::git::PushStatus::Pushed, .ahead_count = 0, .behind_count = 0, .error_message = {}};
  }
  std::filesystem::path repo_dir() const override { return repo_root; }
};

} // namespace

TEST_CASE("AiMessageRepo append/list", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");

  holder::index::FtsIndexer fts(db);
  holder::ai::AiMessageRepo repo(db, &fts);

  holder::model::AiMessage msg1;
  msg1.message_id = "msg-1";
  msg1.thread_id = "thread-1";
  msg1.role = "user";
  msg1.source = "manual";
  msg1.content = "Hello";
  msg1.created_at = 10;

  holder::model::AiMessage msg2;
  msg2.message_id = "msg-2";
  msg2.thread_id = "thread-1";
  msg2.role = "assistant";
  msg2.source = "local";
  msg2.provider = "Ollama";
  msg2.model = "llama";
  msg2.content = "Hi";
  msg2.created_at = 11;
  msg2.prompt_hash = "hash";
  msg2.meta_json = "{\"tokens\": 2}";

  repo.append(msg1);
  repo.append(msg2);

  const auto rel_path = holder::core::ai_message_rel_path("msg-2");
  const auto file_path = project_root / rel_path;
  const auto raw = read_file(file_path);
  REQUIRE(raw.find("---\n") == 0);
  REQUIRE(raw.find("message_id: msg-2") != std::string::npos);
  REQUIRE(raw.find("thread_id: thread-1") != std::string::npos);
  REQUIRE(raw.find("project_id: proj-1") != std::string::npos);
  REQUIRE(raw.rfind("Hi") != std::string::npos);

  const auto list = repo.list_by_thread("thread-1");
  REQUIRE(list.size() == 2);
  REQUIRE(list[0].message_id == "msg-1");
  REQUIRE(list[1].provider.has_value());
  REQUIRE(list[1].meta_json.has_value());
}

TEST_CASE("AiMessageRepo append throws when thread is missing", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1", (dir / "project_repo").string());

  holder::ai::AiMessageRepo repo(db, nullptr);
  holder::model::AiMessage msg;
  msg.message_id = "msg-missing-thread";
  msg.thread_id = "thread-missing";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "Hello";
  msg.created_at = 10;

  REQUIRE_THROWS(repo.append(msg));
}

TEST_CASE("AiMessageRepo append throws on existing file conflict", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");

  const auto rel_path = holder::core::ai_message_rel_path("msg-conflict");
  const auto full_path = project_root / rel_path;
  std::filesystem::create_directories(full_path.parent_path());
  std::ofstream out(full_path);
  REQUIRE(out.is_open());
  out << "existing";
  out.close();

  holder::ai::AiMessageRepo repo(db, nullptr);
  holder::model::AiMessage msg;
  msg.message_id = "msg-conflict";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "Hello";
  msg.created_at = 10;

  REQUIRE_THROWS(repo.append(msg));
}

TEST_CASE("AiMessageRepo uses project remote and deleted_at paths", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  update_project_remote(db, "proj-1", "git@example.com:repo.git", 3);
  create_thread(db, "thread-1", "proj-1");

  TrackingGitOps git;
  holder::ai::AiMessageRepo repo(db, nullptr, nullptr, &git);

  holder::model::AiMessage msg;
  msg.message_id = "msg-remote";
  msg.thread_id = "thread-1";
  msg.role = "assistant";
  msg.source = "manual";
  msg.content = "Remote";
  msg.created_at = 11;
  msg.deleted_at = 99;

  repo.append(msg);
  REQUIRE(git.set_remote_calls >= 1);
}

TEST_CASE("AiMessageRepo trash restore and remove live file", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");

  TrackingGitOps git;
  holder::ai::AiMessageRepo repo(db, nullptr, nullptr, &git);

  holder::model::AiMessage msg;
  msg.message_id = "msg-trash";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "content";
  msg.created_at = 12;
  repo.append(msg);

  repo.trash(msg.message_id, 100);
  auto deleted = repo.list_deleted_by_project("proj-1");
  REQUIRE(deleted.size() == 1);
  REQUIRE_THROWS(repo.trash(msg.message_id, 101));

  repo.restore(msg.message_id);
  deleted = repo.list_deleted_by_project("proj-1");
  REQUIRE(deleted.empty());

  const auto live_rel = holder::core::ai_message_rel_path(msg.message_id);
  const auto live_path = project_root / live_rel;
  REQUIRE(std::filesystem::exists(live_path));

  repo.remove(msg.message_id);
  REQUIRE(!std::filesystem::exists(live_path));
  REQUIRE(std::find(git.removed_paths.begin(), git.removed_paths.end(), live_rel) != git.removed_paths.end());
}

TEST_CASE("AiMessageRepo update_links guards and missing file path", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");

  holder::ai::AiMessageRepo repo(db, nullptr);
  REQUIRE_THROWS(repo.update_links("missing-id"));

  holder::model::AiMessage msg;
  msg.message_id = "msg-links";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "links";
  msg.created_at = 13;
  repo.append(msg);

  const auto rel_path = holder::core::ai_message_rel_path(msg.message_id);
  std::filesystem::remove(project_root / rel_path);
  REQUIRE_THROWS(repo.update_links(msg.message_id));
}

TEST_CASE("AiMessageRepo restore throws when message is not deleted", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");

  holder::ai::AiMessageRepo repo(db, nullptr);
  holder::model::AiMessage msg;
  msg.message_id = "msg-restore";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "restore";
  msg.created_at = 14;
  repo.append(msg);

  REQUIRE_THROWS(repo.restore(msg.message_id));
}

TEST_CASE("AiMessageRepo sqlite prepare failures throw", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  create_project(db, "proj-1", (dir / "project_repo").string());
  create_thread(db, "thread-1", "proj-1");
  holder::ai::AiMessageRepo repo(db, nullptr);

  db.exec("DROP TABLE ai_messages;");

  holder::model::AiMessage msg;
  msg.message_id = "msg-prepare";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "prepare";
  msg.created_at = 15;

  REQUIRE_THROWS(repo.append(msg));
  REQUIRE_THROWS(repo.get("anything"));
  REQUIRE_THROWS(repo.list_by_thread("thread-1"));
  REQUIRE_THROWS(repo.list_deleted_by_project("proj-1"));
}

TEST_CASE("AiMessageRepo append cleans file when DB insert fails", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");
  holder::ai::AiMessageRepo repo(db, nullptr);

  db.exec("CREATE TRIGGER fail_ai_messages_insert BEFORE INSERT ON ai_messages "
          "BEGIN SELECT RAISE(ABORT, 'no insert'); END;");

  holder::model::AiMessage msg;
  msg.message_id = "msg-insert-fail";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "insert fail";
  msg.created_at = 20;

  REQUIRE_THROWS(repo.append(msg));
  const auto full_path = project_root / holder::core::ai_message_rel_path(msg.message_id);
  REQUIRE_FALSE(std::filesystem::exists(full_path));
}

TEST_CASE("AiMessageRepo update throws when thread or project is missing", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");
  holder::ai::AiMessageRepo repo(db, nullptr);

  holder::model::AiMessage msg;
  msg.message_id = "msg-update-guard";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "body";
  msg.created_at = 21;
  repo.append(msg);

  auto no_thread = msg;
  no_thread.thread_id = "missing-thread";
  REQUIRE_THROWS(repo.update(no_thread));

  db.exec("PRAGMA foreign_keys=OFF;");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-bad-project', 'missing-proj', 'T', 1, 1);");
  db.exec("PRAGMA foreign_keys=ON;");
  auto bad_project = msg;
  bad_project.thread_id = "thread-bad-project";
  REQUIRE_THROWS(repo.update(bad_project));
}

TEST_CASE("AiMessageRepo update/trash/restore/remove SQL step failures throw", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");
  holder::ai::AiMessageRepo repo(db, nullptr);

  holder::model::AiMessage msg;
  msg.message_id = "msg-step-fail";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "x";
  msg.created_at = 22;
  repo.append(msg);

  db.exec("CREATE TRIGGER fail_ai_messages_update BEFORE UPDATE ON ai_messages "
          "BEGIN SELECT RAISE(ABORT, 'no update'); END;");
  REQUIRE_THROWS(repo.update(msg));
  REQUIRE_THROWS(repo.trash(msg.message_id, 100));
  REQUIRE_THROWS(repo.restore(msg.message_id));
  db.exec("DROP TRIGGER fail_ai_messages_update;");

  db.exec("CREATE TRIGGER fail_ai_messages_delete BEFORE DELETE ON ai_messages "
          "BEGIN SELECT RAISE(ABORT, 'no delete'); END;");
  REQUIRE_THROWS(repo.remove(msg.message_id));
}

TEST_CASE("AiMessageRepo get/list step error paths via sqlite progress handler", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");
  holder::ai::AiMessageRepo repo(db, nullptr);

  holder::model::AiMessage msg;
  msg.message_id = "msg-progress";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "x";
  msg.created_at = 23;
  repo.append(msg);
  repo.trash(msg.message_id, 200);

  sqlite3_progress_handler(db.handle(), 1, [](void*) -> int { return 1; }, nullptr);
  REQUIRE_THROWS(repo.get(msg.message_id));
  REQUIRE_THROWS(repo.list_by_thread("thread-1"));
  REQUIRE_THROWS(repo.list_deleted_by_project("proj-1"));
  sqlite3_progress_handler(db.handle(), 0, nullptr, nullptr);
}

TEST_CASE("AiMessageRepo trash/restore/update_links throw when thread or project missing", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  create_thread(db, "thread-1", "proj-1");
  holder::ai::AiMessageRepo repo(db, nullptr);

  holder::model::AiMessage msg;
  msg.message_id = "msg-guard-2";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "x";
  msg.created_at = 24;
  repo.append(msg);

  db.exec("PRAGMA foreign_keys=OFF;");
  db.exec("UPDATE ai_messages SET thread_id = 'missing-thread' WHERE message_id = 'msg-guard-2';");
  db.exec("PRAGMA foreign_keys=ON;");
  REQUIRE_THROWS(repo.trash(msg.message_id, 50));
  REQUIRE_THROWS(repo.restore(msg.message_id));
  REQUIRE_THROWS(repo.update_links(msg.message_id));

  db.exec("PRAGMA foreign_keys=OFF;");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-bad-2', 'missing-proj', 'T', 1, 1);");
  db.exec("UPDATE ai_messages SET thread_id = 'thread-bad-2' WHERE message_id = 'msg-guard-2';");
  db.exec("PRAGMA foreign_keys=ON;");
  REQUIRE_THROWS(repo.trash(msg.message_id, 51));
  REQUIRE_THROWS(repo.restore(msg.message_id));
  REQUIRE_THROWS(repo.update_links(msg.message_id));
}

TEST_CASE("AiMessageRepo methods honor project remote set_remote path", "[aimessagerepo]") {
  const auto dir = make_temp_dir();
  holder::platform::Db db;
  db.open(dir / "holder.db");
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  update_project_remote(db, "proj-1", "git@example.com:repo.git", 3);
  create_thread(db, "thread-1", "proj-1");

  TrackingGitOps git;
  holder::ai::AiMessageRepo repo(db, nullptr, nullptr, &git);

  holder::model::AiMessage msg;
  msg.message_id = "msg-remote-all";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "x";
  msg.created_at = 25;
  repo.append(msg);
  repo.update(msg);
  repo.update_links(msg.message_id);
  repo.trash(msg.message_id, 80);
  repo.restore(msg.message_id);
  repo.remove(msg.message_id);

  REQUIRE(git.set_remote_calls >= 5);
}
