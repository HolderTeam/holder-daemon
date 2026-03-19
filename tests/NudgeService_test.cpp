#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/NudgeService.h"
#include "http_test_helpers.h"

namespace {

void create_card_fixture(holder::platform::Db& db,
                         const std::string& project_id,
                         const std::string& card_id) {
  db.exec(
      "INSERT INTO cards(card_id, project_id, title, rel_path, created_at, updated_at) "
      "VALUES('" + card_id + "', '" + project_id + "', 'Fixture', 'cards/" + card_id + ".md', 1, 1);");
}

holder::ai::NudgeCandidateInput title_only_candidate(const std::string& fingerprint) {
  return {
      .kind = "card.title_only",
      .project_id = "proj-1",
      .card_id = std::optional<std::string>("card-1"),
      .created_at = 123,
      .basis_fingerprint = std::optional<std::string>(fingerprint),
      .basis_commit = std::nullopt,
      .facts = {{"title", "Frog"}, {"body_empty", true}, {"doc_chars", 12}, {"body_chars", 0}},
  };
}

} // namespace

TEST_CASE("NudgeService persists dedupe and dismiss across service instances", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1");
  create_card_fixture(db, "proj-1", "card-1");

  holder::ai::NudgeService service1(db);
  auto first = service1.evaluate_and_record(title_only_candidate("fp-1"));
  REQUIRE(first.accepted);
  REQUIRE(first.should_nudge);
  REQUIRE(first.nudge.has_value());

  holder::ai::NudgeService service2(db);
  auto listed = service2.list("proj-1", std::optional<std::string>("card-1"));
  REQUIRE(listed.size() == 1);
  REQUIRE(listed[0].nudge_id == first.nudge->nudge_id);

  auto duplicate = service2.evaluate_and_record(title_only_candidate("fp-1"));
  REQUIRE(duplicate.nudge.has_value());
  REQUIRE(duplicate.nudge->nudge_id == first.nudge->nudge_id);
  REQUIRE(service2.list("proj-1", std::optional<std::string>("card-1")).size() == 1);

  auto replacement = service2.evaluate_and_record(title_only_candidate("fp-2"));
  REQUIRE(replacement.nudge.has_value());
  REQUIRE(replacement.nudge->nudge_id != first.nudge->nudge_id);

  holder::ai::NudgeService service3(db);
  auto after_replace = service3.list("proj-1", std::optional<std::string>("card-1"));
  REQUIRE(after_replace.size() == 1);
  REQUIRE(after_replace[0].nudge_id == replacement.nudge->nudge_id);

  REQUIRE(service3.dismiss(replacement.nudge->nudge_id));

  holder::ai::NudgeService service4(db);
  REQUIRE(service4.list("proj-1", std::optional<std::string>("card-1")).empty());
}
