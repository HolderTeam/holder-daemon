#include "sync/ProjectSyncWorker.h"

#include "core/Signal.h"
#include "git/GitRepo.h"
#include "model/Project.h"
#include "store/ProjectRepo.h"
#include "store/ProjectSyncRepo.h"

#include "http_test_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <optional>
#include <thread>

namespace {

void run_worker_for_seconds(const std::filesystem::path& db_path, int seconds) {
  holder::core::SignalHandler signals;
  holder::sync::ProjectSyncWorker worker(db_path, 2000000000, 1, 1);
  std::thread thread([&worker, &signals]() { worker.run(signals); });
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  std::raise(SIGTERM);
  thread.join();
}

std::optional<long long> load_last_pull_at(const std::filesystem::path& db_path,
                                           const std::string& project_id) {
  holder::store::Db db;
  db.open(db_path);
  holder::store::ProjectSyncRepo sync(db);
  const auto state = sync.get(project_id);
  if (!state.has_value()) {
    return std::nullopt;
  }
  return state->last_pull_at;
}

std::optional<holder::model::ProjectSyncState> load_sync_state(const std::filesystem::path& db_path,
                                                               const std::string& project_id) {
  holder::store::Db db;
  db.open(db_path);
  holder::store::ProjectSyncRepo sync(db);
  return sync.get(project_id);
}

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

TEST_CASE("ProjectSyncWorker periodically refreshes pull timestamp", "[sync][worker]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto remote_dir = dir / "remote_repo";
  const auto local_dir = dir / "local_repo";

  holder::git::GitRepo remote_repo;
  remote_repo.open_or_init(remote_dir);
  remote_repo.write_file("cards/a.md", "hello");
  remote_repo.stage_path("cards/a.md");
  remote_repo.commit("seed");

  {
    auto db = holder::test::open_db_with_schema(db_path);
    holder::store::ProjectRepo projects(db);
    holder::store::ProjectSyncRepo sync(db);
    holder::model::Project project;
    project.project_id = "proj-1";
    project.name = "Project";
    project.root_path = local_dir.string();
    project.git_remote_url = remote_dir.string();
    project.privacy_mode = "plain";
    project.created_at = 1;
    project.updated_at = 1;
    projects.create(project);

    // Seed a recent successful push so push backoff/retry state does not gate pull checks.
    sync.record_push_result(project.project_id, "pushed", true, std::nullopt, now_epoch_seconds());
  }

  run_worker_for_seconds(db_path, 2);
  const auto first_pull_at = load_last_pull_at(db_path, "proj-1");
  REQUIRE(first_pull_at.has_value());

  std::this_thread::sleep_for(std::chrono::seconds(2));

  run_worker_for_seconds(db_path, 2);
  const auto second_pull_at = load_last_pull_at(db_path, "proj-1");
  REQUIRE(second_pull_at.has_value());
  REQUIRE(second_pull_at.value() > first_pull_at.value());
}

TEST_CASE("ProjectSyncWorker pull failure uses pull retry lane", "[sync][worker]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto local_dir = dir / "local_repo";

  holder::git::GitRepo local_repo;
  local_repo.open_or_init(local_dir);
  local_repo.write_file("cards/a.md", "hello");
  local_repo.stage_path("cards/a.md");
  local_repo.commit("seed");

  {
    auto db = holder::test::open_db_with_schema(db_path);
    holder::store::ProjectRepo projects(db);
    holder::store::ProjectSyncRepo sync(db);
    holder::model::Project project;
    project.project_id = "proj-1";
    project.name = "Project";
    project.root_path = local_dir.string();
    project.git_remote_url = (dir / "missing_remote").string();
    project.privacy_mode = "plain";
    project.created_at = 1;
    project.updated_at = 1;
    projects.create(project);
    sync.record_push_result(project.project_id, "pushed", true, std::nullopt, now_epoch_seconds());
  }

  run_worker_for_seconds(db_path, 2);
  const auto state = load_sync_state(db_path, "proj-1");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status.has_value());
  REQUIRE(state->last_pull_status.value() == "failed");
  REQUIRE(state->pull_retry_count >= 1);
  REQUIRE(state->next_pull_retry_at.has_value());
}
