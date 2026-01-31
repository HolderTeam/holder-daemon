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

  repo.update_status("run-1", "completed", std::nullopt, "msg-1", "model-1", std::nullopt, 2);
  const auto updated = repo.get("run-1");
  REQUIRE(updated.has_value());
  REQUIRE(updated->status == "completed");
  REQUIRE(updated->message_id.value() == "msg-1");
  REQUIRE(updated->chosen_model.value() == "model-1");
}
