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
#include "platform/Db.h"
#include "project/ProjectRepo.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

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
