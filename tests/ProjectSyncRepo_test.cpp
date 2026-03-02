#include "store/ProjectRepo.h"
#include "store/ProjectSyncRepo.h"
#include "http_test_helpers.h"

TEST_CASE("ProjectSyncRepo records push retries and clears on success", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::store::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::store::ProjectSyncRepo sync(db);

  sync.record_push_result("proj-1", "auth_failed", false, std::optional<std::string>{"bad auth"}, 100);
  auto first = sync.get("proj-1");
  REQUIRE(first.has_value());
  REQUIRE(first->retry_count == 1);
  REQUIRE(first->next_retry_at.has_value());
  REQUIRE(first->next_retry_at.value() == 160);
  REQUIRE(first->last_sync_error.has_value());
  REQUIRE(first->last_sync_error.value() == "bad auth");
  REQUIRE(first->last_push_status.has_value());
  REQUIRE(first->last_push_status.value() == "auth_failed");

  sync.record_push_result("proj-1", "unknown_error", false, std::optional<std::string>{"still broken"}, 200);
  auto second = sync.get("proj-1");
  REQUIRE(second.has_value());
  REQUIRE(second->retry_count == 2);
  REQUIRE(second->next_retry_at.has_value());
  REQUIRE(second->next_retry_at.value() == 500);

  sync.record_push_result("proj-1", "pushed", true, std::nullopt, 300);
  auto success = sync.get("proj-1");
  REQUIRE(success.has_value());
  REQUIRE(success->retry_count == 0);
  REQUIRE_FALSE(success->next_retry_at.has_value());
  REQUIRE_FALSE(success->last_sync_error.has_value());
  REQUIRE(success->last_push_at.has_value());
  REQUIRE(success->last_push_at.value() == 300);
}

TEST_CASE("ProjectSyncRepo records pull status and errors", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::store::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::store::ProjectSyncRepo sync(db);
  sync.record_pull_result("proj-1", "failed", false, std::optional<std::string>{"ff-only failed"}, 400);
  auto failed = sync.get("proj-1");
  REQUIRE(failed.has_value());
  REQUIRE(failed->last_pull_status.has_value());
  REQUIRE(failed->last_pull_status.value() == "failed");
  REQUIRE(failed->last_sync_error.has_value());
  REQUIRE(failed->last_sync_error.value() == "ff-only failed");
  REQUIRE(failed->last_sync_error_at.has_value());
  REQUIRE(failed->last_sync_error_at.value() == 400);

  sync.record_pull_result("proj-1", "succeeded", true, std::nullopt, 450);
  auto ok = sync.get("proj-1");
  REQUIRE(ok.has_value());
  REQUIRE(ok->last_pull_at.has_value());
  REQUIRE(ok->last_pull_at.value() == 450);
  REQUIRE_FALSE(ok->last_sync_error.has_value());
}

TEST_CASE("ProjectSyncRepo stores uncommitted and unpushed counters", "[sync][repo]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);

  holder::store::ProjectRepo projects(db);
  holder::model::Project project;
  project.project_id = "proj-1";
  project.name = "Project";
  project.root_path = (dir / "repo").string();
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);

  holder::store::ProjectSyncRepo sync(db);
  sync.update_activity_counts("proj-1", 3, 7, 500);

  const auto state = sync.get("proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->uncommitted_changes_count == 3);
  REQUIRE(state->unpushed_commits_count == 7);
}
