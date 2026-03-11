#include "api/support/ThreadCompaction.h"
#include "platform/Db.h"

#include <catch2/matchers/catch_matchers_string.hpp>
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("ThreadCompaction state round-trip and build context", "[thread_compaction]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_thread_compaction";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
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

TEST_CASE("ThreadCompaction normalizes structured summary and accepts quality", "[thread_compaction]") {
  const std::string candidate =
      "## Decisions\n"
      "- Keep cloud-first fallback for low-end devices\n"
      "## Constraints\n"
      "- Avoid user editing yaml files\n"
      "## Open Questions\n"
      "- Should provider order be user-customizable?\n"
      "## Next Actions\n"
      "- Add client-facing bootstrap endpoint\n";

  const auto out = holder::api::support::normalize_and_validate_rolling_summary(
      candidate, std::nullopt, 2000);
  REQUIRE(out.accepted);
  REQUIRE(out.summary.find("## Decisions") != std::string::npos);
  REQUIRE(out.summary.find("## Constraints") != std::string::npos);
  REQUIRE(out.summary.find("## Open Questions") != std::string::npos);
  REQUIRE(out.summary.find("## Next Actions") != std::string::npos);
}

TEST_CASE("ThreadCompaction quality guard rejects low-signal summary", "[thread_compaction]") {
  const auto out = holder::api::support::normalize_and_validate_rolling_summary(
      "ok", std::optional<std::string>("previous summary with substantial details"), 2000);
  REQUIRE_FALSE(out.accepted);
  REQUIRE_FALSE(out.reason.empty());
}

TEST_CASE("ThreadCompaction can fallback-section unstructured summary", "[thread_compaction]") {
  const std::string candidate =
      "Keep cloud-first fallback.\n"
      "Do not require yaml edits by users.\n"
      "Track quota_exceeded separately from rate_limited.\n"
      "Add clearer setup status in clients.\n";

  const auto out = holder::api::support::normalize_and_validate_rolling_summary(
      candidate, std::nullopt, 2000);
  REQUIRE(out.accepted);
  REQUIRE(out.used_fallback_sections);
  REQUIRE(out.summary.find("## Decisions") != std::string::npos);
}

TEST_CASE("ThreadCompaction upsert throws sqlite error when table is missing", "[thread_compaction]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_thread_compaction_missing_table";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-missing";
  state.rolling_summary = "Summary";
  state.updated_at = 1;

  REQUIRE_THROWS_WITH(holder::api::support::upsert_thread_compaction_state(db, state),
                      Catch::Matchers::ContainsSubstring("prepare thread compaction upsert failed"));
}

TEST_CASE("ThreadCompaction load throws sqlite error when table is missing", "[thread_compaction]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_thread_compaction_load_missing_table";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  REQUIRE_THROWS_WITH(holder::api::support::load_thread_compaction_state(db, "thread-missing"),
                      Catch::Matchers::ContainsSubstring("prepare thread compaction get failed"));
}

TEST_CASE("ThreadCompaction upsert binds nullable fields and reads optional last_compacted_message_id",
          "[thread_compaction]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_thread_compaction_nullable_fields";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-2', 'Project', '/tmp/project2', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-2', 'proj-2', 'Thread', 1, 1);");

  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-2";
  state.rolling_summary = std::nullopt;
  state.pinned_facts_json = std::nullopt;
  state.last_compacted_message_id = "msg-123";
  state.updated_at = 42;
  holder::api::support::upsert_thread_compaction_state(db, state);

  const auto loaded = holder::api::support::load_thread_compaction_state(db, "thread-2");
  REQUIRE(loaded.has_value());
  REQUIRE_FALSE(loaded->rolling_summary.has_value());
  REQUIRE_FALSE(loaded->pinned_facts_json.has_value());
  REQUIRE(loaded->last_compacted_message_id == std::optional<std::string>("msg-123"));
}

TEST_CASE("ThreadCompaction upsert throws when sqlite step fails", "[thread_compaction]") {
  const auto dir = std::filesystem::temp_directory_path() / "holder_thread_compaction_step_failure";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-3', 'Project', '/tmp/project3', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-3', 'proj-3', 'Thread', 1, 1);");
  db.exec("CREATE TRIGGER block_compaction_insert "
          "BEFORE INSERT ON ai_thread_compaction_state "
          "BEGIN SELECT RAISE(ABORT, 'blocked'); END;");

  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-3";
  state.rolling_summary = "Summary";
  state.updated_at = 100;

  REQUIRE_THROWS_WITH(holder::api::support::upsert_thread_compaction_state(db, state),
                      Catch::Matchers::ContainsSubstring("upsert thread compaction failed"));
}

TEST_CASE("ThreadCompaction handles zero token budget and oversize prefix truncation", "[thread_compaction]") {
  bool compacted = false;
  bool used_summary = false;
  int pinned_count = 0;

  const std::string zero_budget =
      holder::api::support::build_compacted_context("non-empty", 0, std::nullopt, &compacted, &used_summary,
                                                    &pinned_count);
  REQUIRE(zero_budget.empty());
  REQUIRE(compacted);
  REQUIRE_FALSE(used_summary);
  REQUIRE(pinned_count == 0);

  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-oversize";
  state.rolling_summary = std::string(200, 'S');
  const std::string out =
      holder::api::support::build_compacted_context("tail", 10, state, &compacted, &used_summary, &pinned_count);
  REQUIRE(out.size() <= 40);
  REQUIRE(compacted);
  REQUIRE(used_summary);

  // Force prefix (summary + pinned facts block) to exceed max_bytes so build_compacted_context
  // takes the explicit `out = prefix.substr(0, max_bytes)` truncation branch.
  holder::api::support::ThreadCompactionState state_with_facts;
  state_with_facts.thread_id = "thread-oversize-facts";
  state_with_facts.rolling_summary = std::string(120, 'A');
  state_with_facts.pinned_facts_json =
      R"(["Fact 1 that is intentionally long to consume bytes","Fact 2 that is intentionally long to consume bytes","Fact 3 that is intentionally long to consume bytes"])";
  const std::string out_with_facts = holder::api::support::build_compacted_context(
      "tail", 10, state_with_facts, &compacted, &used_summary, &pinned_count);
  REQUIRE(out_with_facts.size() == 40);
  REQUIRE(compacted);
  REQUIRE(used_summary);
  REQUIRE(pinned_count >= 1);
}

TEST_CASE("ThreadCompaction robustly handles malformed pinned facts inputs", "[thread_compaction]") {
  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-malformed";
  state.pinned_facts_json = "{ not json";

  bool compacted = false;
  bool used_summary = false;
  int pinned_count = 0;
  const std::string built = holder::api::support::build_compacted_context(
      R"({"still":"ok"})", 128, state, &compacted, &used_summary, &pinned_count);
  REQUIRE_FALSE(built.empty());
  REQUIRE(pinned_count == 0);

  const auto dir = std::filesystem::temp_directory_path() / "holder_thread_compaction_invalid_context_json";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  std::ifstream in(SCHEMA_SQL_PATH);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-4', 'Project', '/tmp/project4', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-4', 'proj-4', 'Thread', 1, 1);");

  holder::api::support::roll_thread_compaction_state(db, "thread-4", "{ broken json", 1234);
  const auto loaded = holder::api::support::load_thread_compaction_state(db, "thread-4");
  REQUIRE(loaded.has_value());
}

TEST_CASE("ThreadCompaction validates low-signal, numbered bullets, and regressive shrink",
          "[thread_compaction]") {
  const auto low_signal = holder::api::support::normalize_and_validate_rolling_summary(
      "This is one long line but it still has only one content item and no sections.", std::nullopt, 2000);
  REQUIRE_FALSE(low_signal.accepted);
  REQUIRE(low_signal.reason == "low_signal");

  const std::string numbered_candidate =
      "1. Keep retries bounded\n"
      "2. Prefer explicit provider selection\n"
      "3. Surface actionable errors to clients\n";
  const auto numbered =
      holder::api::support::normalize_and_validate_rolling_summary(numbered_candidate, std::nullopt, 2000);
  REQUIRE(numbered.accepted);
  REQUIRE(numbered.used_fallback_sections);
  REQUIRE(numbered.summary.find("- Keep retries bounded") != std::string::npos);

  const std::string structured =
      "## Decisions\n"
      "- Keep cloud fallback\n"
      "## Constraints\n"
      "- No yaml edits\n"
      "## Open Questions\n"
      "- How to expose retries?\n"
      "## Next Actions\n"
      "- Add endpoint docs\n";
  const std::string previous(500, 'P');
  const auto shrink = holder::api::support::normalize_and_validate_rolling_summary(
      structured, std::optional<std::string>(previous), 50);
  REQUIRE_FALSE(shrink.accepted);
  REQUIRE(shrink.reason == "regressive_shrink");
}
