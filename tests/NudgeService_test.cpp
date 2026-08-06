#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiLocalModelConfigRepo.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiRunnerRepo.h"
#include "ai/AiThreadRepo.h"
#include "ai/NudgeService.h"
#include "card/CardRepo.h"
#include "git/GitRepo.h"
#include "http_test_helpers.h"
#include "llm/LocalModelRunner.h"
#include "llm/LocalRunnerClient.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"

#include <git2.h>
#include <openssl/sha.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace holder::ai {

struct NudgeServiceTestAccess {
  static holder::ai::NudgeDecision evaluate_candidate(const holder::ai::NudgeCandidateInput& input
  ) {
    return holder::ai::NudgeService::evaluate_candidate(input);
  }

  static std::string build_nudge_title(const holder::ai::NudgeCandidateInput& input) {
    return holder::ai::NudgeService::build_nudge_title(input);
  }

  static std::string build_nudge_body(const holder::ai::NudgeCandidateInput& input) {
    return holder::ai::NudgeService::build_nudge_body(input);
  }

  static std::optional<std::string> load_card_body(
      holder::platform::Db& db,
      const std::string& project_id,
      const std::string& card_id
  ) {
    return holder::ai::NudgeService::access_load_card_body(db, project_id, card_id);
  }

  static std::vector<std::string> sibling_card_titles(
      holder::platform::Db& db,
      const std::string& project_id,
      const std::string& card_id
  ) {
    return holder::ai::NudgeService::access_sibling_card_titles(db, project_id, card_id);
  }

  static std::vector<holder::model::Card> sibling_cards(
      holder::platform::Db& db,
      const std::string& project_id,
      const std::string& card_id
  ) {
    return holder::ai::NudgeService::access_sibling_cards(db, project_id, card_id);
  }

  static std::string card_excerpt_line(
      holder::platform::Db& db,
      const std::string& project_id,
      const holder::model::Card& card
  ) {
    return holder::ai::NudgeService::access_card_excerpt_line(db, project_id, card);
  }

  static std::vector<std::string> recent_project_card_excerpts(
      holder::platform::Db& db,
      const std::string& project_id,
      const std::optional<std::string>& exclude_card_id,
      std::size_t limit
  ) {
    return holder::ai::NudgeService::access_recent_project_card_excerpts(
        db,
        project_id,
        exclude_card_id,
        limit
    );
  }

  static std::optional<std::string> current_card_fingerprint(
      holder::platform::Db& db,
      const std::string& project_id,
      const std::string& card_id
  ) {
    return holder::ai::NudgeService::current_card_fingerprint(db, project_id, card_id);
  }

  static std::optional<std::string> current_project_head_commit(
      holder::platform::Db& db,
      const std::string& project_id
  ) {
    return holder::ai::NudgeService::current_project_head_commit(db, project_id);
  }
};

} // namespace holder::ai

namespace {

void create_card_fixture(
    holder::platform::Db& db,
    const std::string& project_id,
    const std::string& card_id
) {
  db.exec(
      "INSERT INTO cards(card_id, project_id, title, rel_path, created_at, updated_at) "
      "VALUES('" +
      card_id + "', '" + project_id + "', 'Fixture', 'cards/" + card_id + ".md', 1, 1);"
  );
}

void insert_card_fixture(
    holder::platform::Db& db,
    const std::string& project_id,
    const std::string& card_id,
    const std::string& title,
    const std::string& rel_path,
    long long updated_at,
    const std::optional<std::string>& parent_card_id = std::nullopt
) {
  const std::string parent_sql = parent_card_id.has_value() ? ("'" + parent_card_id.value() + "'")
                                                            : "NULL";
  db.exec(
      "INSERT INTO cards(card_id, project_id, title, rel_path, parent_card_id, created_at, "
      "updated_at) VALUES('" +
      card_id + "', '" + project_id + "', '" + title + "', '" + rel_path + "', " + parent_sql +
      ", 1, " + std::to_string(updated_at) + ");"
  );
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

holder::ai::NudgeCandidateInput title_suggestion_candidate(const std::string& fingerprint) {
  return {
      .kind = "card.title_suggestion",
      .project_id = "proj-1",
      .card_id = std::optional<std::string>("card-1"),
      .created_at = 123,
      .basis_fingerprint = std::optional<std::string>(fingerprint),
      .basis_commit = std::nullopt,
      .facts =
          {{"title", "Untitled"}, {"body_empty", false}, {"doc_chars", 180}, {"body_chars", 160}},
  };
}

std::string short_content_fingerprint(const std::string& content) {
  unsigned char digest[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(content.data()), content.size(), digest);
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

void write_card_markdown(
    const std::filesystem::path& project_root,
    const std::string& rel_path,
    const std::string& title,
    const std::string& body
) {
  write_text(project_root / rel_path, "# " + title + "\n\n" + body);
}

std::string current_head_commit(const std::filesystem::path& repo_path) {
  git_repository* repo = nullptr;
  const auto repo_path_string = repo_path.string();
  REQUIRE(git_repository_open(&repo, repo_path_string.c_str()) == 0);
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

void insert_ai_message(
    holder::platform::Db& db,
    const std::string& message_id,
    const std::string& thread_id,
    const std::string& role,
    const std::string& content,
    long long created_at
) {
  db.exec(
      "INSERT INTO ai_messages(message_id, thread_id, role, source, provider, model, content, "
      "created_at, deleted_at, prompt_hash, meta_json) VALUES('" +
      message_id + "', '" + thread_id + "', '" + role + "', 'test', NULL, NULL, '" + content +
      "', " + std::to_string(created_at) + ", NULL, NULL, NULL);"
  );
}

std::string capture_nudge_prompt(
    holder::platform::Db& db,
    const holder::ai::NudgeCandidateInput& input
) {
  holder::llm::LocalModelRunner runner;
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models.push_back({.name = "fake-echo", .digest = "", .size = 1, .modified_at = ""});
  runner.set_status_override_for_tests(status);

  std::string captured_prompt;
  runner.set_stream_generate_override_for_tests([&](const std::string&,
                                                    const std::string& prompt,
                                                    const std::string&,
                                                    const std::function<void(const std::string&)>&,
                                                    std::string* error) {
    captured_prompt = prompt;
    if (error) *error = "force deterministic fallback";
    return false;
  });

  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::ai::NudgeService service(db, &runner_registry);
  const auto decision = service.evaluate_and_record(input);
  REQUIRE(decision.accepted);
  REQUIRE(decision.should_nudge);
  REQUIRE(decision.nudge.has_value());
  return captured_prompt;
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

TEST_CASE("NudgeService creates title suggestion nudges with three options", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir / "cards");
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", repo_dir.string());
  insert_card_fixture(db, "proj-1", "card-1", "Untitled", "cards/card-1.md", 1);
  write_card_markdown(
      repo_dir,
      "cards/card-1.md",
      "Untitled",
      "Frogs use wetlands as practical nurseries. Their eggs and tadpoles depend on shallow water, shelter, and seasonal changes."
  );

  holder::ai::NudgeService service(db);
  const auto decision = service.evaluate_and_record(title_suggestion_candidate("fp-title-1"));
  REQUIRE(decision.accepted);
  REQUIRE(decision.should_nudge);
  REQUIRE(decision.reason == "title_suggestion_candidate_ready");
  REQUIRE(decision.nudge.has_value());
  REQUIRE(decision.nudge->kind == "card.title_suggestion");
  REQUIRE(decision.nudge->title == "Suggest a title");
  REQUIRE(decision.nudge->meta_json.is_object());
  REQUIRE(decision.nudge->meta_json["suggestions"].is_array());
  REQUIRE(decision.nudge->meta_json["suggestions"].size() == 3);

  const auto duplicate = service.evaluate_and_record(title_suggestion_candidate("fp-title-1"));
  REQUIRE(duplicate.nudge.has_value());
  REQUIRE(duplicate.nudge->nudge_id == decision.nudge->nudge_id);
}

TEST_CASE(
    "NudgeService does not recreate dismissed exact title suggestion nudges",
    "[ai][nudges]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir / "cards");
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", repo_dir.string());
  insert_card_fixture(db, "proj-1", "card-1", "Untitled", "cards/card-1.md", 1);
  write_card_markdown(
      repo_dir,
      "cards/card-1.md",
      "Untitled",
      "Owls hunt quietly at night, using soft-edged feathers and sharp hearing."
  );

  holder::ai::NudgeService service(db);
  const auto first = service.evaluate_and_record(title_suggestion_candidate("fp-title-dismissed"));
  REQUIRE(first.accepted);
  REQUIRE(first.should_nudge);
  REQUIRE(first.nudge.has_value());
  REQUIRE(service.dismiss(first.nudge->nudge_id));

  const auto duplicate = service.evaluate_and_record(title_suggestion_candidate("fp-title-dismissed"
  ));
  REQUIRE(duplicate.accepted);
  REQUIRE_FALSE(duplicate.should_nudge);
  REQUIRE(duplicate.reason == "nudge_already_dismissed");
  REQUIRE_FALSE(duplicate.nudge.has_value());
  REQUIRE(service.list("proj-1", std::optional<std::string>("card-1")).empty());
}

TEST_CASE("NudgeService parses fenced JSON title suggestions from runner", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir / "cards");
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", repo_dir.string());
  insert_card_fixture(db, "proj-1", "card-1", "Untitled", "cards/card-1.md", 1);
  write_card_markdown(
      repo_dir,
      "cards/card-1.md",
      "Untitled",
      "In Holder, io_threads and general_workers serve different stages of the request pipeline."
  );

  holder::llm::LocalModelRunner runner;
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models.push_back({.name = "fake-title", .digest = "", .size = 1, .modified_at = ""});
  runner.set_status_override_for_tests(status);
  runner.set_stream_generate_override_for_tests([](const std::string&,
                                                   const std::string&,
                                                   const std::string&,
                                                   const std::function<void(const std::string&)>&
                                                       on_chunk,
                                                   std::string*) {
    on_chunk(
        "```json\n[\n  \"Understanding Threads in Holder\",\n  \"Request Pipeline Stages\",\n  \"Async Worker Roles\"\n]\n```"
    );
    return true;
  });

  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::ai::NudgeService service(db, &runner_registry);
  const auto decision = service.evaluate_and_record(title_suggestion_candidate("fp-title-json"));

  REQUIRE(decision.nudge.has_value());
  const auto suggestions = decision.nudge->meta_json["suggestions"];
  REQUIRE(suggestions.size() == 3);
  REQUIRE(suggestions[0] == "Understanding Threads in Holder");
  REQUIRE(suggestions[1] == "Request Pipeline Stages");
  REQUIRE(suggestions[2] == "Async Worker Roles");
}

TEST_CASE("NudgeService uses bare JSON title suggestions from runner", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir / "cards");
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", repo_dir.string());
  insert_card_fixture(db, "proj-1", "card-1", "Untitled", "cards/card-1.md", 1);
  write_card_markdown(
      repo_dir,
      "cards/card-1.md",
      "Untitled",
      "In Holder, io_threads and general_workers serve different stages of the request pipeline."
  );

  holder::llm::LocalModelRunner runner;
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models.push_back({.name = "fake-title", .digest = "", .size = 1, .modified_at = ""});
  runner.set_status_override_for_tests(status);
  runner.set_stream_generate_override_for_tests(
      [](const std::string&,
         const std::string&,
         const std::string&,
         const std::function<void(const std::string&)>& on_chunk,
         std::string*) {
        on_chunk("[\"\\\"Thread Roles in Holder\\\"\", \"Request Pipeline Stages\", "
                 "\"Socket Mechanics vs App Work\"]");
        return true;
      }
  );

  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::ai::NudgeService service(db, &runner_registry);
  const auto decision = service.evaluate_and_record(title_suggestion_candidate("fp-title-bare-json")
  );

  REQUIRE(decision.nudge.has_value());
  const auto suggestions = decision.nudge->meta_json["suggestions"];
  REQUIRE(suggestions.size() == 3);
  REQUIRE(suggestions[0] == "Thread Roles in Holder");
  REQUIRE(suggestions[1] == "Request Pipeline Stages");
  REQUIRE(suggestions[2] == "Socket Mechanics vs App Work");
}

TEST_CASE("NudgeService cleans plain-line title suggestions from runner", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir / "cards");
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", repo_dir.string());
  insert_card_fixture(db, "proj-1", "card-1", "Untitled", "cards/card-1.md", 1);
  write_card_markdown(
      repo_dir,
      "cards/card-1.md",
      "Untitled",
      "In Holder, io_threads and general_workers serve different stages of the request pipeline."
  );

  holder::llm::LocalModelRunner runner;
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models.push_back({.name = "fake-title", .digest = "", .size = 1, .modified_at = ""});
  runner.set_status_override_for_tests(status);
  runner.set_stream_generate_override_for_tests(
      [](const std::string&,
         const std::string&,
         const std::string&,
         const std::function<void(const std::string&)>& on_chunk,
         std::string*) {
        on_chunk("* \"Thread Roles in Holder\"\n"
                 "2) Request Pipeline Management.\n"
                 "This title is deliberately longer than sixty characters so it truncates cleanly\n"
        );
        return true;
      }
  );

  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::ai::NudgeService service(db, &runner_registry);
  const auto decision = service.evaluate_and_record(title_suggestion_candidate("fp-title-lines"));

  REQUIRE(decision.nudge.has_value());
  const auto suggestions = decision.nudge->meta_json["suggestions"];
  REQUIRE(suggestions.size() == 3);
  REQUIRE(suggestions[0] == "Thread Roles in Holder");
  REQUIRE(suggestions[1] == "Request Pipeline Management");
  REQUIRE(suggestions[2].get<std::string>().size() == 60);
  REQUIRE(suggestions[2].get<std::string>().rfind("This title is deliberately longer", 0) == 0);
}

TEST_CASE("NudgeService falls back to generic titles for blank loaded body", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir / "cards");
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", repo_dir.string());
  insert_card_fixture(db, "proj-1", "card-1", "Untitled", "cards/card-1.md", 1);
  write_text(repo_dir / "cards" / "card-1.md", "---\ntitle: Untitled\n---\n     \n       \n");

  holder::ai::NudgeService service(db);
  const auto decision = service.evaluate_and_record(title_suggestion_candidate("fp-title-blank-body"
  ));

  REQUIRE(decision.nudge.has_value());
  const auto suggestions = decision.nudge->meta_json["suggestions"];
  REQUIRE(suggestions.size() == 3);
  REQUIRE(suggestions[0] == "Card Summary");
  REQUIRE(suggestions[1] == "Key Ideas");
  REQUIRE(suggestions[2] == "Title Option 3");
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
    auto created = service.evaluate_and_record(
        title_only_candidate(short_content_fingerprint(original))
    );
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
        // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
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
        }
    );

    holder::llm::LocalRunnerClient local_runner_client(&runner);
    holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
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
    runner.set_stream_generate_override_for_tests([](const std::string&,
                                                     const std::string&,
                                                     const std::string&,
                                                     const std::function<void(const std::string&)>&,
                                                     std::string* error) {
      if (error) *error = "boom";
      return false;
    });

    holder::llm::LocalRunnerClient local_runner_client(&runner);
    holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
    holder::ai::NudgeService service(db, &runner_registry);
    auto decision = service.evaluate_and_record(title_only_candidate("fp-2"));
    REQUIRE(decision.nudge.has_value());
    REQUIRE(
        decision.nudge->body ==
        "You named this card \"Frog\" but it still only has a title. Draft an opening paragraph or a short outline next."
    );
  }

  SECTION("configured manual fast model does not fall back to auto-local runner") {
    holder::test::EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "1");
    holder::ai::AiRunnerRepo(db).upsert(holder::model::AiRunner{
        .runner_id = "manual-a",
        .name = "Office Ollama",
        .kind = "ollama",
        .base_url = std::optional<std::string>("http://office:11434"),
        .source = "manual",
        .enabled = true,
        .created_at = 1,
        .updated_at = 1,
    });
    holder::ai::AiLocalModelConfigRepo(db)
        .set(std::string("manual-a::fake-echo"), std::nullopt, std::nullopt, 2);

    holder::llm::LocalModelRunner runner;
    holder::llm::RunnerStatus status;
    status.available = true;
    status.models.push_back({.name = "fake-echo", .digest = "", .size = 1, .modified_at = ""});
    runner.set_status_override_for_tests(status);

    bool used_auto_local_for_nudge = false;
    runner.set_stream_generate_override_for_tests(
        [&](const std::string&,
            const std::string& prompt,
            const std::string&,
            const std::function<void(const std::string&)>&,
            std::string*) {
          if (prompt.find("Rewrite this app nudge") != std::string::npos) {
            used_auto_local_for_nudge = true;
          }
          return false;
        }
    );

    holder::llm::LocalRunnerClient local_runner_client(&runner);
    holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
    auto* manual_client = runner_registry.get_client("manual-a");
    REQUIRE(manual_client != nullptr);
    (void)manual_client->retry();

    holder::ai::NudgeService service(db, &runner_registry);
    auto decision = service.evaluate_and_record(title_only_candidate("fp-3"));
    REQUIRE(decision.nudge.has_value());
    REQUIRE_FALSE(used_auto_local_for_nudge);
    REQUIRE(
        decision.nudge->body ==
        "You named this card \"Frog\" but it still only has a title. Draft an opening paragraph or a short outline next."
    );
  }
}

TEST_CASE("NudgeService includes rich card and AI context in runner prompt", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir / "cards");

  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", repo_dir.string());

  insert_card_fixture(db, "proj-1", "parent-1", "Parent Topic", "cards/parent.md", 50);
  insert_card_fixture(
      db,
      "proj-1",
      "card-1",
      "Frog",
      "cards/card-1.md",
      80,
      std::optional<std::string>("parent-1")
  );
  insert_card_fixture(
      db,
      "proj-1",
      "sib-1",
      "Sibling One",
      "cards/sibling-one.md",
      70,
      std::optional<std::string>("parent-1")
  );
  insert_card_fixture(
      db,
      "proj-1",
      "sib-2",
      "",
      "cards/sibling-empty.md",
      65,
      std::optional<std::string>("parent-1")
  );
  insert_card_fixture(
      db,
      "proj-1",
      "sib-3",
      "Missing Body",
      "cards/missing-body.md",
      60,
      std::optional<std::string>("parent-1")
  );
  insert_card_fixture(db, "proj-1", "recent-1", "Recent Note", "cards/recent.md", 120);
  insert_card_fixture(db, "proj-1", "recent-2", "Older Note", "cards/older.md", 110);

  write_card_markdown(
      repo_dir,
      "cards/parent.md",
      "Parent Topic",
      "  Parent background context that should appear in the excerpt.  \n"
  );
  write_card_markdown(
      repo_dir,
      "cards/sibling-one.md",
      "Sibling One",
      "   Sibling body with enough detail to appear as an excerpt and exercise "
      "trimming at both ends.   \n"
  );
  write_card_markdown(repo_dir, "cards/sibling-empty.md", "", "   \n");
  write_card_markdown(
      repo_dir,
      "cards/recent.md",
      "Recent Note",
      "   Recent project note with useful context and a long enough body to be "
      "picked up in the prompt.   \n"
  );
  write_card_markdown(repo_dir, "cards/older.md", "Older Note", "   Older note body.   \n");

  holder::ai::AiThreadRepo thread_repo(db);
  thread_repo.create({
      .thread_id = "thread-card",
      .project_id = "proj-1",
      .card_id = std::optional<std::string>("card-1"),
      .title = "Thread for Frog",
      .created_at = 10,
      .updated_at = 200,
  });
  insert_ai_message(db, "msg-1", "thread-card", "user", "  First draft question.  ", 10);
  insert_ai_message(db, "msg-2", "thread-card", "assistant", "  First answer.  ", 11);
  insert_ai_message(db, "msg-3", "thread-card", "user", "  Follow-up question.  ", 12);
  insert_ai_message(
      db,
      "msg-4",
      "thread-card",
      "assistant",
      "  " + std::string(260, 'A') + "  ",
      13
  );
  insert_ai_message(db, "msg-5", "thread-card", "user", "  Final user question.  ", 14);

  const auto prompt = capture_nudge_prompt(
      db,
      {
          .kind = "card.title_only",
          .project_id = "proj-1",
          .card_id = std::optional<std::string>("card-1"),
          .created_at = 123,
          .basis_fingerprint = std::optional<std::string>("fp-rich"),
          .basis_commit = std::nullopt,
          .facts = {{"title", "Frog"}, {"body_empty", true}, {"doc_chars", 12}, {"body_chars", 0}},
      }
  );

  CAPTURE(prompt);
  REQUIRE(prompt.find("Current card title: Frog") != std::string::npos);
  REQUIRE(prompt.find("Sibling cards: Sibling One; Missing Body") != std::string::npos);
  REQUIRE(prompt.find("Parent card title: Parent Topic") != std::string::npos);
  REQUIRE(prompt.find("Parent card excerpt: # Parent Topic") != std::string::npos);
  REQUIRE(
      prompt.find("Parent background context that should appear in the excerpt.") !=
      std::string::npos
  );
  REQUIRE(prompt.find("Sibling card excerpts:\n- Sibling One: # Sibling One") != std::string::npos);
  REQUIRE(
      prompt.find("Recent project card excerpts:\n- Recent Note: # Recent Note") !=
      std::string::npos
  );
  REQUIRE(prompt.find("Recent AI thread:\nAssistant: ") != std::string::npos);
  REQUIRE(prompt.find("User: Final user question.") != std::string::npos);
  REQUIRE(prompt.find("Current card body:") == std::string::npos);
  REQUIRE(prompt.find("First draft question.") == std::string::npos);
}

TEST_CASE("NudgeService handles encrypted card context in runner prompt", "[ai][nudges][privacy]") {
  const auto dir = holder::test::make_temp_dir();
  const auto repo_dir = dir / "repo";
  const auto keystore_dir = dir / "keystore";
  std::filesystem::create_directories(repo_dir / "cards");
  std::filesystem::create_directories(keystore_dir);
  holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::project::ProjectRepo project_repo(db);
  holder::model::Project project;
  project.project_id = "proj-enc";
  project.name = "Encrypted";
  project.root_path = repo_dir.string();
  project.privacy_mode = "encrypted_git";
  project.created_at = 1;
  project.updated_at = 1;
  project_repo.create(project);

  holder::git::RealGitOps git;
  holder::privacy::ensure_encrypted_project_ready(
      git,
      project_repo,
      project.project_id,
      project.root_path,
      project.project_key_id,
      project.updated_at,
      []() {
        return std::string("enc-key");
      }
  );

  insert_card_fixture(db, "proj-enc", "card-1", "Secret", "cards/card-1.md", 10);
  const auto stored_project = project_repo.get("proj-enc");
  REQUIRE(stored_project.has_value());
  REQUIRE(stored_project->project_key_id.has_value());

  auto write_encrypted_card = [&](const std::string& key_id, const std::string& plaintext) {
    const auto envelope = holder::privacy::encrypt_project_blob("proj-enc", key_id, plaintext);
    write_text(repo_dir / "cards" / "card-1.md", envelope);
  };

  const holder::ai::NudgeCandidateInput input{
      .kind = "card.title_only",
      .project_id = "proj-enc",
      .card_id = std::optional<std::string>("card-1"),
      .created_at = 123,
      .basis_fingerprint = std::optional<std::string>("fp-enc"),
      .basis_commit = std::nullopt,
      .facts = {{"title", "Secret"}, {"body_empty", true}, {"doc_chars", 12}, {"body_chars", 0}},
  };

  SECTION("valid encrypted card body is included") {
    write_encrypted_card(
        stored_project->project_key_id.value(),
        "# Secret\n\n  Confidential details that should be decrypted.  \n"
    );
    const auto prompt = capture_nudge_prompt(db, input);
    CAPTURE(prompt);
    REQUIRE(prompt.find("Current card body:\n# Secret") != std::string::npos);
    REQUIRE(prompt.find("Confidential details that should be decrypted.") != std::string::npos);
  }

  SECTION("missing project key id suppresses encrypted body context") {
    write_encrypted_card(
        stored_project->project_key_id.value(),
        "# Secret\n\n  Confidential details that should be decrypted.  \n"
    );
    project_repo.update_project_key_id("proj-enc", std::nullopt, 2);
    const auto prompt = capture_nudge_prompt(db, input);
    CAPTURE(prompt);
    REQUIRE(prompt.find("Current card body:") == std::string::npos);
  }

  SECTION("decrypt failure suppresses encrypted body context") {
    project_repo.update_project_key_id("proj-enc", std::optional<std::string>("wrong-key"), 2);
    write_encrypted_card(
        stored_project->project_key_id.value(),
        "# Secret\n\n  Confidential details that should be decrypted.  \n"
    );
    const auto prompt = capture_nudge_prompt(db, input);
    CAPTURE(prompt);
    REQUIRE(prompt.find("Current card body:") == std::string::npos);
  }
}

TEST_CASE("NudgeService covers helper edge cases and runner fallback behaviour", "[ai][nudges]") {
  SECTION("unknown candidate kind uses default decision and wording") {
    const holder::ai::NudgeCandidateInput input{
        .kind = "unknown.kind",
        .project_id = "proj-1",
        .card_id = std::nullopt,
        .created_at = 1,
        .basis_fingerprint = std::nullopt,
        .basis_commit = std::nullopt,
        .facts = nlohmann::json::object(),
    };

    const auto decision = holder::ai::NudgeServiceTestAccess::evaluate_candidate(input);
    REQUIRE_FALSE(decision.accepted);
    REQUIRE_FALSE(decision.should_nudge);
    REQUIRE(decision.reason == "unknown_candidate_kind");
    REQUIRE(holder::ai::NudgeServiceTestAccess::build_nudge_title(input) == "Suggestion");
    REQUIRE(
        holder::ai::NudgeServiceTestAccess::build_nudge_body(input) == "No suggestion available."
    );
  }

  SECTION("current card fingerprint and project head commit handle missing inputs") {
    const auto dir = holder::test::make_temp_dir();
    auto db = holder::test::open_db_with_schema(dir / "holder.db");

    REQUIRE_FALSE(holder::ai::NudgeServiceTestAccess::current_card_fingerprint(
                      db,
                      "missing-project",
                      "missing-card"
    )
                      .has_value());
    REQUIRE_FALSE(
        holder::ai::NudgeServiceTestAccess::current_project_head_commit(db, "missing-project")
            .has_value()
    );
  }

  SECTION("direct helper access covers missing-card and empty-excerpt branches") {
    const auto dir = holder::test::make_temp_dir();
    const auto repo_dir = dir / "repo";
    std::filesystem::create_directories(repo_dir / "cards");
    auto db = holder::test::open_db_with_schema(dir / "holder.db");
    holder::test::create_project(db, "proj-1", repo_dir.string());

    REQUIRE_FALSE(
        holder::ai::NudgeServiceTestAccess::load_card_body(db, "missing-project", "missing-card")
            .has_value()
    );
    REQUIRE(holder::ai::NudgeServiceTestAccess::sibling_card_titles(db, "proj-1", "missing-card")
                .empty());
    REQUIRE(holder::ai::NudgeServiceTestAccess::sibling_cards(db, "proj-1", "missing-card").empty()
    );

    insert_card_fixture(db, "proj-1", "card-1", "Focus", "cards/card-1.md", 50);
    insert_card_fixture(db, "proj-1", "blank-card", "", "cards/blank-card.md", 40);
    write_text(repo_dir / "cards" / "blank-card.md", "   \n   \n");

    const auto blank = holder::card::CardRepo(db).get("blank-card");
    REQUIRE(blank.has_value());
    REQUIRE(
        holder::ai::NudgeServiceTestAccess::card_excerpt_line(db, "proj-1", blank.value()).empty()
    );

    const auto recent = holder::ai::NudgeServiceTestAccess::recent_project_card_excerpts(
        db,
        "proj-1",
        std::nullopt,
        8
    );
    REQUIRE(recent.empty());
  }

  SECTION("prompt building handles a missing card id without extra context") {
    const auto dir = holder::test::make_temp_dir();
    const auto repo_dir = dir / "repo";
    std::filesystem::create_directories(repo_dir / "cards");
    auto db = holder::test::open_db_with_schema(dir / "holder.db");
    holder::test::create_project(db, "proj-1", repo_dir.string());

    holder::llm::LocalModelRunner runner;
    holder::llm::RunnerStatus status;
    status.available = true;
    status.models.push_back({.name = "fake-echo", .digest = "", .size = 1, .modified_at = ""});
    runner.set_status_override_for_tests(status);

    std::string captured_prompt;
    runner.set_stream_generate_override_for_tests(
        [&](const std::string&,
            const std::string& prompt,
            const std::string&,
            const std::function<void(const std::string&)>&,
            std::string* error) {
          captured_prompt = prompt;
          if (error) *error = "force deterministic fallback";
          return false;
        }
    );

    holder::llm::LocalRunnerClient local_runner_client(&runner);
    holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
    holder::ai::NudgeService service(db, &runner_registry);
    const auto decision = service.evaluate_and_record({
        .kind = "card.title_only",
        .project_id = "proj-1",
        .card_id = std::nullopt,
        .created_at = 11,
        .basis_fingerprint = std::optional<std::string>("fp-no-card"),
        .basis_commit = std::nullopt,
        .facts = {{"title", "Ghost"}, {"body_empty", true}, {"doc_chars", 12}, {"body_chars", 0}},
    });

    REQUIRE(decision.nudge.has_value());
    CAPTURE(captured_prompt);
    REQUIRE(captured_prompt.find("Current card title:") == std::string::npos);
    REQUIRE(captured_prompt.find("Sibling cards:") == std::string::npos);
    REQUIRE(captured_prompt.find("Sibling card excerpts:") == std::string::npos);
  }

  SECTION("current project head commit handles invalid repo and unborn HEAD") {
    const auto dir = holder::test::make_temp_dir();
    auto db = holder::test::open_db_with_schema(dir / "holder.db");

    const auto invalid_repo = dir / "not-a-git-repo";
    std::filesystem::create_directories(invalid_repo);
    holder::test::create_project(db, "proj-invalid", invalid_repo.string());
    REQUIRE_FALSE(
        holder::ai::NudgeServiceTestAccess::current_project_head_commit(db, "proj-invalid")
            .has_value()
    );

    const auto unborn_repo = dir / "unborn-repo";
    holder::git::GitRepo repo;
    repo.open_or_init(unborn_repo);
    holder::test::create_project(db, "proj-unborn", unborn_repo.string());
    REQUIRE_FALSE(holder::ai::NudgeServiceTestAccess::current_project_head_commit(db, "proj-unborn")
                      .has_value());
  }

  SECTION("current card fingerprint handles encrypted missing key and decrypt failure") {
    const auto dir = holder::test::make_temp_dir();
    const auto repo_dir = dir / "repo";
    const auto keystore_dir = dir / "keystore";
    std::filesystem::create_directories(repo_dir / "cards");
    std::filesystem::create_directories(keystore_dir);
    holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

    auto db = holder::test::open_db_with_schema(dir / "holder.db");
    holder::project::ProjectRepo project_repo(db);
    holder::model::Project project;
    project.project_id = "proj-enc";
    project.name = "Encrypted";
    project.root_path = repo_dir.string();
    project.privacy_mode = "encrypted_git";
    project.created_at = 1;
    project.updated_at = 1;
    project_repo.create(project);

    holder::git::RealGitOps git;
    holder::privacy::ensure_encrypted_project_ready(
        git,
        project_repo,
        project.project_id,
        project.root_path,
        project.project_key_id,
        project.updated_at,
        []() {
          return std::string("enc-key");
        }
    );

    insert_card_fixture(db, "proj-enc", "card-1", "Secret", "cards/card-1.md", 10);
    const auto stored_project = project_repo.get("proj-enc");
    REQUIRE(stored_project.has_value());
    REQUIRE(stored_project->project_key_id.has_value());

    const auto envelope = holder::privacy::encrypt_project_blob(
        "proj-enc",
        stored_project->project_key_id.value(),
        "# Secret\n\nEncrypted text.\n"
    );
    write_text(repo_dir / "cards" / "card-1.md", envelope);

    project_repo.update_project_key_id("proj-enc", std::nullopt, 2);
    REQUIRE_FALSE(
        holder::ai::NudgeServiceTestAccess::current_card_fingerprint(db, "proj-enc", "card-1")
            .has_value()
    );

    project_repo.update_project_key_id("proj-enc", std::optional<std::string>("wrong-key"), 3);
    REQUIRE_FALSE(
        holder::ai::NudgeServiceTestAccess::current_card_fingerprint(db, "proj-enc", "card-1")
            .has_value()
    );
  }

  SECTION("current card fingerprint handles encrypted successful decrypt") {
    const auto dir = holder::test::make_temp_dir();
    const auto repo_dir = dir / "repo";
    const auto keystore_dir = dir / "keystore";
    std::filesystem::create_directories(repo_dir / "cards");
    std::filesystem::create_directories(keystore_dir);
    holder::test::EnvGuard keystore_env("HOLDER_TEST_KEYSTORE_DIR", keystore_dir.string());

    auto db = holder::test::open_db_with_schema(dir / "holder.db");
    holder::project::ProjectRepo project_repo(db);
    holder::model::Project project;
    project.project_id = "proj-enc";
    project.name = "Encrypted";
    project.root_path = repo_dir.string();
    project.privacy_mode = "encrypted_git";
    project.created_at = 1;
    project.updated_at = 1;
    project_repo.create(project);

    holder::git::RealGitOps git;
    holder::privacy::ensure_encrypted_project_ready(
        git,
        project_repo,
        project.project_id,
        project.root_path,
        project.project_key_id,
        project.updated_at,
        []() {
          return std::string("enc-key");
        }
    );

    insert_card_fixture(db, "proj-enc", "card-1", "Secret", "cards/card-1.md", 10);
    const auto stored_project = project_repo.get("proj-enc");
    REQUIRE(stored_project.has_value());
    REQUIRE(stored_project->project_key_id.has_value());

    const std::string plain = "# Secret\n\nEncrypted text.\n";
    const auto envelope = holder::privacy::encrypt_project_blob(
        "proj-enc",
        stored_project->project_key_id.value(),
        plain
    );
    write_text(repo_dir / "cards" / "card-1.md", envelope);

    const auto fingerprint =
        holder::ai::NudgeServiceTestAccess::current_card_fingerprint(db, "proj-enc", "card-1");
    REQUIRE(fingerprint.has_value());
    REQUIRE(fingerprint.value() == short_content_fingerprint(plain));
  }

  SECTION("prompt context covers missing card fallback sibling limits and empty AI thread") {
    const auto dir = holder::test::make_temp_dir();
    const auto repo_dir = dir / "repo";
    std::filesystem::create_directories(repo_dir / "cards");

    auto db = holder::test::open_db_with_schema(dir / "holder.db");
    holder::test::create_project(db, "proj-1", repo_dir.string());

    insert_card_fixture(db, "proj-1", "card-1", "Focus", "cards/card-1.md", 50);
    insert_card_fixture(db, "proj-1", "other-card", "Other", "cards/other-card.md", 40);
    write_card_markdown(repo_dir, "cards/other-card.md", "Other", "Other thread body.");
    insert_card_fixture(db, "proj-1", "blank-card", "", "cards/blank-card.md", 39);
    write_text(repo_dir / "cards" / "blank-card.md", "   \n   \n");

    for (int i = 0; i < 10; ++i) {
      const auto id = "sib-" + std::to_string(i);
      const auto rel = "cards/" + id + ".md";
      insert_card_fixture(db, "proj-1", id, "Sibling " + std::to_string(i), rel, 100 - i);
      write_card_markdown(
          repo_dir,
          rel,
          "Sibling " + std::to_string(i),
          "Sibling excerpt " + std::to_string(i) + "."
      );
    }

    insert_card_fixture(db, "proj-1", "recent-empty", "Recent Empty", "cards/recent-empty.md", 200);
    write_card_markdown(repo_dir, "cards/recent-empty.md", "Recent Empty", "   \n");

    holder::ai::AiThreadRepo thread_repo(db);
    thread_repo.create({
        .thread_id = "thread-other",
        .project_id = "proj-1",
        .card_id = std::optional<std::string>("other-card"),
        .title = "Other thread",
        .created_at = 1,
        .updated_at = 2,
    });

    const auto prompt = capture_nudge_prompt(
        db,
        {
            .kind = "card.title_only",
            .project_id = "proj-1",
            .card_id = std::optional<std::string>("card-1"),
            .created_at = 123,
            .basis_fingerprint = std::optional<std::string>("fp-limits"),
            .basis_commit = std::nullopt,
            .facts =
                {{"title", "Focus"}, {"body_empty", true}, {"doc_chars", 12}, {"body_chars", 0}},
        }
    );

    CAPTURE(prompt);
    REQUIRE(prompt.find("Current card body:") == std::string::npos);
    REQUIRE(prompt.find("Sibling cards: ") != std::string::npos);
    REQUIRE(prompt.find("Recent Empty") != std::string::npos);
    REQUIRE(prompt.find("Sibling 0") != std::string::npos);
    REQUIRE(prompt.find("Sibling 6") != std::string::npos);
    REQUIRE(prompt.find("Sibling 7") == std::string::npos);
    REQUIRE(prompt.find("Sibling 8") == std::string::npos);
    REQUIRE(prompt.find("Sibling card excerpts:\n") != std::string::npos);
    REQUIRE(prompt.find("- Recent Empty: # Recent Empty") != std::string::npos);
    REQUIRE(prompt.find("- Sibling 0: # Sibling 0") != std::string::npos);
    REQUIRE(prompt.find("- Sibling 2: # Sibling 2") != std::string::npos);
    REQUIRE(prompt.find("- Sibling 3: # Sibling 3") == std::string::npos);
    REQUIRE(prompt.find("Recent project card excerpts:") != std::string::npos);
    REQUIRE(prompt.find("blank-card") == std::string::npos);
    REQUIRE(prompt.find("Recent AI thread:") == std::string::npos);
  }

  SECTION("runner auto-pick prefers smaller positive model and rejects bad rewrites") {
    const auto dir = holder::test::make_temp_dir();
    auto db = holder::test::open_db_with_schema(dir / "holder.db");
    holder::test::create_project(db, "proj-1");
    create_card_fixture(db, "proj-1", "card-1");

    holder::llm::LocalModelRunner runner;
    holder::llm::RunnerStatus status;
    status.available = true;
    status.models.push_back({.name = "unknown-size", .digest = "", .size = 0, .modified_at = ""});
    status.models.push_back({.name = "small", .digest = "", .size = 2, .modified_at = ""});
    status.models.push_back({.name = "bigger", .digest = "", .size = 5, .modified_at = ""});
    runner.set_status_override_for_tests(status);

    std::string chosen_model;
    runner.set_stream_generate_override_for_tests(
        [&](const std::string& model,
            const std::string&,
            const std::string&,
            const std::function<void(const std::string&)>& on_chunk,
            std::string*) {
          chosen_model = model;
          if (model == "small") {
            on_chunk(std::string(300, 'x'));
            return true;
          }
          return false;
        }
    );

    holder::llm::LocalRunnerClient local_runner_client(&runner);
    holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
    holder::ai::NudgeService service(db, &runner_registry);
    const auto decision = service.evaluate_and_record(title_only_candidate("fp-long"));
    REQUIRE(decision.nudge.has_value());
    REQUIRE(chosen_model == "small");
    REQUIRE(decision.nudge->body.find("You named this card") != std::string::npos);

    runner.set_stream_generate_override_for_tests(
        [&](const std::string& model,
            const std::string&,
            const std::string&,
            const std::function<void(const std::string&)>& on_chunk,
            std::string*) {
          chosen_model = model;
          on_chunk("\"");
          return true;
        }
    );

    const auto second = service.evaluate_and_record(title_only_candidate("fp-quote"));
    REQUIRE(second.nudge.has_value());
    REQUIRE(chosen_model == "small");
    REQUIRE(second.nudge->body.find("You named this card") != std::string::npos);

    runner.set_stream_generate_override_for_tests(
        [&](const std::string& model,
            const std::string&,
            const std::string&,
            const std::function<void(const std::string&)>& on_chunk,
            std::string*) {
          chosen_model = model;
          on_chunk("Current card: do this next.");
          return true;
        }
    );

    const auto third = service.evaluate_and_record(title_only_candidate("fp-banned"));
    REQUIRE(third.nudge.has_value());
    REQUIRE(chosen_model == "small");
    REQUIRE(third.nudge->body.find("You named this card") != std::string::npos);
  }
}
