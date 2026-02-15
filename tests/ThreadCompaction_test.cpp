#include "api/support/ThreadCompaction.h"
#include "store/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("ThreadCompaction state round-trip and build context", "[thread_compaction]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_thread_compaction";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-1";
  state.rolling_summary = "Prior summary";
  state.pinned_facts_json = R"(["Always mention UTC","Project uses C++20"])";
  state.updated_at = 100;
  holder::api::support::upsert_thread_compaction_state(db, state);

  const auto loaded = holder::api::support::load_thread_compaction_state(db, "thread-1");
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->rolling_summary.has_value());
  REQUIRE(loaded->pinned_facts_json.has_value());

  bool compacted = false;
  bool used_summary = false;
  int pinned_count = 0;
  const std::string context = std::string(8000, 'x');
  const std::string built = holder::api::support::build_compacted_context(
      context, 500, loaded, &compacted, &used_summary, &pinned_count);
  REQUIRE_FALSE(built.empty());
  REQUIRE(compacted);
  REQUIRE(used_summary);
  REQUIRE(pinned_count >= 1);
  REQUIRE_FALSE(built.empty());

  holder::api::support::roll_thread_compaction_state(
      db, "thread-1", R"({"pinned_facts":["Keep responses concise"],"blob":"abc"})", 200);
  const auto rolled = holder::api::support::load_thread_compaction_state(db, "thread-1");
  REQUIRE(rolled.has_value());
  REQUIRE(rolled->updated_at == 200);
  REQUIRE(rolled->rolling_summary.has_value());
  REQUIRE(rolled->rolling_summary->find("blob") != std::string::npos);
  REQUIRE(rolled->pinned_facts_json.has_value());
  REQUIRE(rolled->pinned_facts_json->find("Keep responses concise") != std::string::npos);
}
