#include "store/AiRunRepo.h"
#include "store/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

TEST_CASE("AiRunRepo create/get/update", "[db]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_ai_runs";
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
  db.exec("INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
          "VALUES('msg-1', 'thread-1', 'assistant', 'local', 'hi', 1);");

  holder::store::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-1";
  run.project_id = "proj-1";
  run.thread_id = "thread-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "started";
  run.created_at = 1;
  run.updated_at = 1;
  repo.create(run);

  const auto fetched = repo.get("run-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->run_id == "run-1");
  REQUIRE(fetched->status == "started");

  repo.update_status("run-1",
                     "completed",
                     std::nullopt,
                     "msg-1",
                     "model-1",
                     std::nullopt,
                     std::optional<std::string>(R"({"path":"cloud","result":{"status":"completed"}})"),
                     2);
  const auto updated = repo.get("run-1");
  REQUIRE(updated.has_value());
  REQUIRE(updated->status == "completed");
  REQUIRE(updated->message_id.value() == "msg-1");
  REQUIRE(updated->chosen_model.value() == "model-1");
  REQUIRE(updated->policy_trace_json.has_value());
}
