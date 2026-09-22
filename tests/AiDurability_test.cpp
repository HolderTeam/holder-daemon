#include "ai/AiNudgeDurability.h"
#include "ai/AiNudgeRepo.h"
#include "ai/AiThreadDurability.h"
#include "ai/AiThreadRepo.h"
#include "ai/AiThreadStateDurability.h"
#include "http_test_helpers.h"
#include "project/ProjectStore.h"
#include <catch2/matchers/catch_matchers_string.hpp>
#include <sqlite3.h>

namespace {
void check_no_busy_statements(holder::platform::Db& db) {
  // FTS retains idle internal prepared statements. An interrupted application
  // query must not retain an active statement or its read transaction.
  for (auto* statement = sqlite3_next_stmt(db.handle(), nullptr); statement;
       statement = sqlite3_next_stmt(db.handle(), statement)) {
    INFO(sqlite3_sql(statement));
    CHECK_FALSE(sqlite3_stmt_busy(statement));
  }
}

struct DurabilityFixture {
  std::filesystem::path root = holder::test::make_temp_dir();
  holder::test::EnvGuard keys{"HOLDER_TEST_KEYSTORE_DIR", (root / "keys").string()};
  holder::platform::Db db = holder::test::open_db_with_schema(root / "holder.db");
  holder::model::Project project;
  explicit DurabilityFixture(bool encrypted = false) {
    holder::model::Project input;
    input.project_id = "project";
    input.name = "Durability";
    input.privacy_mode = encrypted ? "encrypted_git" : "plain";
    input.created_at = input.updated_at = 10;
    project = holder::project::ProjectStore(db).create(
        input,
        [] {
          return "unused";
        },
        root / "projects"
    );
    holder::model::AiThread thread;
    thread.thread_id = "thread-one";
    thread.project_id = project.project_id;
    thread.title = "Conversation";
    thread.created_at = thread.updated_at = 11;
    holder::ai::AiThreadRepo(db).create(thread);
  }
  std::filesystem::path state_path() const {
    return std::filesystem::path(project.root_path) / "ai_thread_state/th/re/thread-one.json";
  }
};
} // namespace

TEST_CASE("AI durable state backfills and restores optional fields", "[durability]") {
  bool encrypted = false;
  SECTION("plain") {}
  SECTION("encrypted") { encrypted = true; }
  DurabilityFixture f(encrypted);
  f.db.exec(
      "INSERT INTO ai_thread_compaction_state VALUES('thread-one','summary','[]','message-last',12)"
  );
  CHECK_FALSE(holder::ai::all_thread_compaction_states_are_durable(f.db));
  CHECK(holder::ai::backfill_thread_compaction_states(f.db) == 1);
  CHECK(holder::ai::all_thread_compaction_states_are_durable(f.db));
  CHECK(holder::ai::backfill_thread_compaction_states(f.db) == 0);
  f.db.exec("DELETE FROM ai_thread_compaction_state");
  CHECK(holder::ai::restore_thread_compaction_states(f.db) == 1);
  const auto restored = holder::api::support::load_thread_compaction_state(f.db, "thread-one");
  REQUIRE(restored.has_value());
  CHECK(restored->rolling_summary == "summary");
  CHECK(restored->pinned_facts_json == "[]");
  CHECK(restored->last_compacted_message_id == "message-last");
  CHECK(restored->updated_at == 12);
  CHECK(holder::ai::persist_thread_compaction_state(f.db, {"thread-one", {}, {}, {}, 13}));
  CHECK(holder::ai::restore_thread_compaction_states(f.db) == 1);
  const auto cleared = holder::api::support::load_thread_compaction_state(f.db, "thread-one");
  REQUIRE(cleared.has_value());
  CHECK_FALSE(cleared->rolling_summary.has_value());
  CHECK_FALSE(cleared->pinned_facts_json.has_value());
  CHECK_FALSE(cleared->last_compacted_message_id.has_value());
}

TEST_CASE(
    "AI durable state rejects corrupt files and releases failed SQL statements",
    "[durability]"
) {
  DurabilityFixture f;
  auto state = nlohmann::json{{"version", 1}, {"thread_id", "thread-one"}, {"updated_at", 12}};
  std::string expected;
  SECTION("unsupported version") {
    state["version"] = 2;
    expected = "unsupported";
  }
  SECTION("empty thread id") {
    state["thread_id"] = "";
    expected = "no thread_id";
  }
  SECTION("mismatched filename") {
    state["thread_id"] = "thread-two";
    expected = "does not match";
  }
  SECTION("missing thread") {
    f.db.exec("DELETE FROM ai_threads");
    expected = "unknown thread";
  }
  SECTION("prepare failure") {
    f.db.exec("DROP TABLE ai_thread_compaction_state");
    expected = "prepare";
  }
  SECTION("write failure") {
    f.db.exec(
        "CREATE TRIGGER reject_state BEFORE INSERT ON ai_thread_compaction_state BEGIN SELECT RAISE(ABORT, 'test'); END"
    );
    expected = "restore AI thread state failed";
  }
  std::filesystem::create_directories(f.state_path().parent_path());
  std::ofstream(f.state_path()) << state.dump();
  REQUIRE_THROWS_WITH(
      holder::ai::restore_thread_compaction_states(f.db),
      Catch::Matchers::ContainsSubstring(expected)
  );
  check_no_busy_statements(f.db);
}

TEST_CASE("AI durability backfill failures release their active statements", "[durability]") {
  DurabilityFixture f;
  bool nudge = false;
  SECTION("thread state backfill") {}
  SECTION("nudge dismissal backfill") { nudge = true; }
  if (nudge) {
    f.db.exec(
        "INSERT INTO ai_nudges(nudge_id,kind,project_id,title,body,meta_json,created_at,dismissed_at) VALUES('x','test','project','Title','Body','{}',1,2)"
    );
    REQUIRE_THROWS(holder::ai::backfill_nudge_dismissals(f.db));
    check_no_busy_statements(f.db);
    REQUIRE_THROWS(holder::ai::all_nudge_dismissals_are_durable(f.db));
  } else {
    f.db.exec("UPDATE ai_threads SET thread_id='x'");
    f.db.exec("INSERT INTO ai_thread_compaction_state VALUES('x',NULL,NULL,NULL,12)");
    REQUIRE_THROWS(holder::ai::backfill_thread_compaction_states(f.db));
    check_no_busy_statements(f.db);
    REQUIRE_THROWS(holder::ai::all_thread_compaction_states_are_durable(f.db));
  }
  check_no_busy_statements(f.db);
}

TEST_CASE("AI nudge dismissals backfill and restore encrypted and plain metadata", "[durability]") {
  bool encrypted = false;
  SECTION("plain") {}
  SECTION("encrypted") { encrypted = true; }
  DurabilityFixture f(encrypted);
  f.db.exec(
      "INSERT INTO ai_nudges(nudge_id,kind,project_id,title,body,meta_json,basis_fingerprint,basis_commit,created_at,dismissed_at) VALUES('nudge-one','test','project','Title','Body','{}','fingerprint','commit',1,2)"
  );
  CHECK_FALSE(holder::ai::all_nudge_dismissals_are_durable(f.db));
  CHECK(holder::ai::backfill_nudge_dismissals(f.db) == 1);
  CHECK(holder::ai::all_nudge_dismissals_are_durable(f.db));
  CHECK(holder::ai::backfill_nudge_dismissals(f.db) == 0);
  f.db.exec("DELETE FROM ai_nudges");
  CHECK(holder::ai::restore_nudge_dismissals(f.db) == 1);
  const auto restored = holder::ai::AiNudgeRepo(f.db).find_by_id("nudge-one");
  REQUIRE(restored.has_value());
  CHECK(restored->dismissed);
  CHECK(restored->title == "Title");
  CHECK(restored->body == "Body");
  CHECK(restored->basis_fingerprint == "fingerprint");
  CHECK(restored->basis_commit == "commit");
}

TEST_CASE("AI durability reports missing schemas and encryption keys", "[durability]") {
  DurabilityFixture f;
  SECTION("schema unavailable") {
    f.db.exec("DROP TABLE ai_thread_compaction_state; DROP TABLE ai_nudges");
    REQUIRE_THROWS(holder::ai::backfill_thread_compaction_states(f.db));
    REQUIRE_THROWS(holder::ai::all_thread_compaction_states_are_durable(f.db));
    REQUIRE_THROWS(holder::ai::persist_nudge_dismissal(f.db, "missing"));
    REQUIRE_THROWS(holder::ai::backfill_nudge_dismissals(f.db));
    REQUIRE_THROWS(holder::ai::all_nudge_dismissals_are_durable(f.db));
  }
  SECTION("encryption key absent") {
    f.db.exec("UPDATE projects SET privacy_mode='encrypted_git', project_key_id=NULL");
    REQUIRE_THROWS(holder::ai::persist_thread_compaction_state(f.db, {"thread-one", {}, {}, {}, 12})
    );
    std::filesystem::create_directories(f.state_path().parent_path());
    std::ofstream(f.state_path()) << "encrypted";
    REQUIRE_THROWS(holder::ai::restore_thread_compaction_states(f.db));
    f.db.exec(
        "INSERT INTO ai_nudges(nudge_id,kind,project_id,title,body,meta_json,created_at,dismissed_at) VALUES('nudge-one','test','project','Title','Body','{}',1,2)"
    );
    REQUIRE_THROWS(holder::ai::persist_nudge_dismissal(f.db, "nudge-one"));
    const auto path = std::filesystem::path(f.project.root_path) /
                      "ai_nudge_dismissals/nu/dg/nudge-one.json";
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "encrypted";
    REQUIRE_THROWS(holder::ai::restore_nudge_dismissals(f.db));
  }
  check_no_busy_statements(f.db);
}

TEST_CASE("AI nudge restore rejects corrupt tombstones and SQL failures", "[durability]") {
  DurabilityFixture f;
  nlohmann::json body = {
      {"version", 1},
      {"nudge_id", "nudge-one"},
      {"project_id", "project"},
      {"kind", "test"},
      {"title", "Title"},
      {"body", "Body"},
      {"meta_json", nlohmann::json::object()},
      {"card_id", nullptr},
      {"basis_fingerprint", nullptr},
      {"basis_commit", nullptr},
      {"created_at", 1},
      {"dismissed_at", 2}
  };
  std::string expected;
  SECTION("version") {
    body["version"] = 2;
    expected = "unsupported";
  }
  SECTION("invalid dismissal") {
    body["dismissed_at"] = 0;
    expected = "invalid";
  }
  SECTION("wrong project") {
    body["project_id"] = "another";
    expected = "mismatch";
  }
  SECTION("wrong filename") {
    body["nudge_id"] = "nudge-two";
    expected = "mismatch";
  }
  SECTION("prepare fails") {
    f.db.exec("DROP TABLE ai_nudges");
    expected = "prepare";
  }
  SECTION("insert fails") {
    f.db.exec(
        "CREATE TRIGGER reject_nudge BEFORE INSERT ON ai_nudges BEGIN SELECT RAISE(ABORT, 'test'); END"
    );
    expected = "restore nudge dismissal failed";
  }
  const auto path = std::filesystem::path(f.project.root_path) /
                    "ai_nudge_dismissals/nu/dg/nudge-one.json";
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << body.dump();
  REQUIRE_THROWS_WITH(
      holder::ai::restore_nudge_dismissals(f.db),
      Catch::Matchers::ContainsSubstring(expected)
  );
  check_no_busy_statements(f.db);
}

TEST_CASE("AI thread manifest backfill only writes missing manifests", "[durability]") {
  DurabilityFixture f;
  CHECK(holder::ai::backfill_ai_thread_manifests(f.db) == 1);
  CHECK(holder::ai::backfill_ai_thread_manifests(f.db) == 0);
  CHECK_FALSE(holder::ai::persist_nudge_dismissal(f.db, "missing"));
}
