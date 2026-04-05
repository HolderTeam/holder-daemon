#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/NudgeService.h"
#include "git/GitRepo.h"
#include "http_test_helpers.h"
#include "llm/LocalModelRunner.h"

#include <git2.h>
#include <openssl/sha.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

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

std::string short_content_fingerprint(const std::string& content) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(content.data()),
         content.size(),
         digest);
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (int i = 0; i < 6; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(digest[i]);
  }
  return out.str();
}

void write_text(const std::filesystem::path& path, const std::string& content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << content;
}

std::string current_head_commit(const std::filesystem::path& repo_path) {
  git_repository* repo = nullptr;
  REQUIRE(git_repository_open(&repo, repo_path.c_str()) == 0);
  git_reference* head = nullptr;
  REQUIRE(git_repository_head(&head, repo) == 0);
  const git_oid* oid = git_reference_target(head);
  REQUIRE(oid != nullptr);
  const char* text = git_oid_tostr_s(oid);
  REQUIRE(text != nullptr);
  std::string out(text);
  git_reference_free(head);
  git_repository_free(repo);
  return out;
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

TEST_CASE("NudgeService prunes stale nudges on list", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();

  SECTION("card fingerprint mismatch dismisses stale nudge") {
    const auto repo_dir = dir / "repo";
    std::filesystem::create_directories(repo_dir / "cards");

    auto db = holder::test::open_db_with_schema(dir / "holder.db");
    holder::test::create_project(db, "proj-1", repo_dir.string());
    create_card_fixture(db, "proj-1", "card-1");

    const std::string original = "# Frog\n\n";
    const std::string updated = "# Frog\n\nNow with body.\n";
    write_text(repo_dir / "cards" / "card-1.md", updated);

    holder::ai::NudgeService service(db);
    auto created = service.evaluate_and_record(title_only_candidate(short_content_fingerprint(original)));
    REQUIRE(created.accepted);
    REQUIRE(created.should_nudge);
    REQUIRE(created.nudge.has_value());

    auto listed = service.list("proj-1", std::optional<std::string>("card-1"));
    REQUIRE(listed.empty());

    holder::ai::NudgeService fresh_service(db);
    REQUIRE(fresh_service.list("proj-1", std::optional<std::string>("card-1")).empty());
  }

  SECTION("project head mismatch dismisses stale git nudge") {
    const auto repo_dir = dir / "repo-git";
    holder::git::GitRepo repo;
    repo.open_or_init(repo_dir);
    repo.write_file("README.md", "first\n");
    repo.stage_path("README.md");
    repo.commit("first");
    const auto first_head = current_head_commit(repo_dir);

    auto db = holder::test::open_db_with_schema(dir / "holder-git.db");
    holder::test::create_project(db, "proj-1", repo_dir.string());

    holder::ai::NudgeService service(db);
    holder::ai::NudgeCandidateInput input{
        .kind = "git.push_failed_repeated",
        .project_id = "proj-1",
        .card_id = std::nullopt,
        .created_at = 123,
        .basis_fingerprint = std::nullopt,
        .basis_commit = first_head,
        .facts = {{"failure_count", 3}, {"latest_status", "auth_failed"}, {"branch", "main"}},
    };
    auto created = service.evaluate_and_record(input);
    REQUIRE(created.accepted);
    REQUIRE(created.should_nudge);
    REQUIRE(created.nudge.has_value());

    repo.write_file("README.md", "second\n");
    repo.stage_path("README.md");
    repo.commit("second");

    auto listed = service.list("proj-1");
    REQUIRE(listed.empty());

    holder::ai::NudgeService fresh_service(db);
    REQUIRE(fresh_service.list("proj-1").empty());
  }
}

TEST_CASE("NudgeService can use local runner wording with deterministic fallback", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1");
  create_card_fixture(db, "proj-1", "card-1");

  SECTION("runner-generated wording is used when available") {
    holder::llm::LocalModelRunner runner;
    holder::llm::RunnerStatus status;
    status.available = true;
    status.models.push_back({.name = "fake-echo", .digest = "", .size = 1, .modified_at = ""});
    runner.set_status_override_for_tests(status);
    runner.set_stream_generate_override_for_tests(
        [](const std::string& model,
           const std::string& prompt,
           const std::string& options_json,
           const std::function<void(const std::string&)>& on_chunk,
           std::string* error) {
          (void)prompt;
          (void)options_json;
          (void)error;
          if (model != "fake-echo") return false;
          on_chunk("Try drafting the first two sentences next.");
          return true;
        });

    holder::llm::RunnerRegistry runner_registry(&db, &runner);
    holder::ai::NudgeService service(db, &runner_registry);
    auto decision = service.evaluate_and_record(title_only_candidate("fp-1"));
    REQUIRE(decision.nudge.has_value());
    REQUIRE(decision.nudge->body == "Try drafting the first two sentences next.");
  }

  SECTION("runner failure falls back to deterministic wording") {
    holder::llm::LocalModelRunner runner;
    holder::llm::RunnerStatus status;
    status.available = true;
    status.models.push_back({.name = "fake-echo", .digest = "", .size = 1, .modified_at = ""});
    runner.set_status_override_for_tests(status);
    runner.set_stream_generate_override_for_tests(
        [](const std::string&,
           const std::string&,
           const std::string&,
           const std::function<void(const std::string&)>&,
           std::string* error) {
          if (error) *error = "boom";
          return false;
        });

    holder::llm::RunnerRegistry runner_registry(&db, &runner);
    holder::ai::NudgeService service(db, &runner_registry);
    auto decision = service.evaluate_and_record(title_only_candidate("fp-2"));
    REQUIRE(decision.nudge.has_value());
    REQUIRE(decision.nudge->body ==
            "You named this card \"Frog\" but it still only has a title. Draft an opening paragraph or a short outline next.");
  }
}
