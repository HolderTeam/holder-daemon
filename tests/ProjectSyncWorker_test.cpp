#include "sync/ProjectSyncWorker.h"

#include "git/GitRepo.h"
#include "model/Project.h"
#include "platform/Signal.h"
#include "project/ProjectRepo.h"
#include "project/ProjectSyncRepo.h"

#include "http_test_helpers.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <csignal>
#include <optional>
#include <thread>

namespace {

class SyncWorkerHookGuard {
 public:
  SyncWorkerHookGuard() = default;
  ~SyncWorkerHookGuard() {
    holder::sync::ProjectSyncWorker::set_fail_post_pull_metrics_for_tests(false);
    holder::sync::ProjectSyncWorker::set_fail_post_push_metrics_for_tests(false);
  }
};

void run_worker_for_seconds(const std::filesystem::path& db_path, int seconds) {
  holder::core::SignalHandler signals;
  holder::sync::ProjectSyncWorker worker(db_path, 2000000000, 1, 1);
  std::thread thread([&worker, &signals]() {
    worker.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  std::raise(SIGTERM);
  thread.join();
}

void run_worker_with_intervals_for_seconds(
    const std::filesystem::path& db_path,
    int push_interval_seconds,
    int pull_interval_seconds,
    int poll_interval_seconds,
    int seconds
) {
  holder::core::SignalHandler signals;
  holder::sync::ProjectSyncWorker
      worker(db_path, push_interval_seconds, pull_interval_seconds, poll_interval_seconds);
  std::thread thread([&worker, &signals]() {
    worker.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  std::raise(SIGTERM);
  thread.join();
}

std::optional<long long> load_last_pull_at(
    const std::filesystem::path& db_path,
    const std::string& project_id
) {
  holder::platform::Db db;
  db.open(db_path);
  holder::project::ProjectSyncRepo sync(db);
  const auto state = sync.get(project_id);
  if (!state.has_value()) {
    return std::nullopt;
  }
  return state->last_pull_at;
}

std::optional<holder::model::ProjectSyncState> load_sync_state(
    const std::filesystem::path& db_path,
    const std::string& project_id
) {
  holder::platform::Db db;
  db.open(db_path);
  holder::project::ProjectSyncRepo sync(db);
  return sync.get(project_id);
}

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}

void create_project(
    holder::project::ProjectRepo& projects,
    const std::string& project_id,
    const std::filesystem::path& root_path,
    const std::string& remote,
    const std::string& privacy_mode = "plain"
) {
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path.string();
  project.git_remote_url = remote;
  project.privacy_mode = privacy_mode;
  project.created_at = 1;
  project.updated_at = 1;
  projects.create(project);
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
    holder::project::ProjectRepo projects(db);
    holder::project::ProjectSyncRepo sync(db);
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
    holder::project::ProjectRepo projects(db);
    holder::project::ProjectSyncRepo sync(db);
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

TEST_CASE("ProjectSyncWorker push cycle records successful push", "[sync][worker]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto remote_dir = dir / "remote_repo";
  const auto local_dir = dir / "local_repo";

  holder::git::GitRepo remote_repo;
  remote_repo.open_or_init(remote_dir);

  holder::git::GitRepo local_repo;
  local_repo.open_or_init(local_dir);
  local_repo.write_file("cards/a.md", "hello");
  local_repo.stage_path("cards/a.md");
  local_repo.commit("seed");

  {
    auto db = holder::test::open_db_with_schema(db_path);
    holder::project::ProjectRepo projects(db);
    holder::project::ProjectSyncRepo sync(db);
    create_project(projects, "proj-push", local_dir, remote_dir.string(), "plain");
    // Skip startup pull path; this test targets push-cycle push handling.
    sync.record_pull_result("proj-push", "succeeded", true, std::nullopt, now_epoch_seconds());
  }

  run_worker_with_intervals_for_seconds(db_path, 0, 3600, 1, 2);
  const auto state = load_sync_state(db_path, "proj-push");
  REQUIRE(state.has_value());
  REQUIRE(state->last_push_status.has_value());
  REQUIRE_FALSE(state->last_push_status.value().empty());
}

TEST_CASE("ProjectSyncWorker push cycle records pull failure", "[sync][worker]") {
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
    holder::project::ProjectRepo projects(db);
    holder::project::ProjectSyncRepo sync(db);
    create_project(
        projects,
        "proj-pull-fail",
        local_dir,
        (dir / "missing_remote").string(),
        "plain"
    );
    // Make startup skip pull, then force run_push_cycle pull attempt.
    sync.record_pull_result(
        "proj-pull-fail",
        "succeeded",
        true,
        std::nullopt,
        now_epoch_seconds() - 10000
    );
  }

  run_worker_with_intervals_for_seconds(db_path, 3600, 1, 1, 2);
  const auto state = load_sync_state(db_path, "proj-pull-fail");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status.has_value());
  REQUIRE(state->last_pull_status.value() == "failed");
}

TEST_CASE("ProjectSyncWorker encrypted project push safety failure is recorded", "[sync][worker]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto remote_dir = dir / "remote_repo";
  const auto local_dir = dir / "local_repo";

  holder::git::GitRepo remote_repo;
  remote_repo.open_or_init(remote_dir);

  holder::git::GitRepo local_repo;
  local_repo.open_or_init(local_dir);
  // Plaintext card blob should fail encrypted push safety check.
  local_repo.write_file("cards/a.md", "# plain\n");
  local_repo.stage_path("cards/a.md");
  local_repo.commit("seed");

  {
    auto db = holder::test::open_db_with_schema(db_path);
    holder::project::ProjectRepo projects(db);
    holder::project::ProjectSyncRepo sync(db);
    create_project(projects, "proj-encrypted", local_dir, remote_dir.string(), "encrypted_git");
    sync.record_pull_result("proj-encrypted", "succeeded", true, std::nullopt, now_epoch_seconds());
  }

  run_worker_with_intervals_for_seconds(db_path, 0, 3600, 1, 2);
  const auto state = load_sync_state(db_path, "proj-encrypted");
  REQUIRE(state.has_value());
  REQUIRE(state->last_push_status.has_value());
  REQUIRE(state->last_push_status.value() == "unknown_error");
  REQUIRE(state->last_sync_error.has_value());
}

TEST_CASE("ProjectSyncWorker skips project when metrics refresh fails", "[sync][worker]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto invalid_root = dir / "not_a_repo_dir";
  const auto remote_dir = dir / "remote_repo";
  std::ofstream marker(invalid_root);
  REQUIRE(marker.good());
  marker << "file";
  marker.close();

  holder::git::GitRepo remote_repo;
  remote_repo.open_or_init(remote_dir);

  {
    auto db = holder::test::open_db_with_schema(db_path);
    holder::project::ProjectRepo projects(db);
    create_project(projects, "proj-invalid-root", invalid_root, remote_dir.string(), "plain");
  }

  run_worker_with_intervals_for_seconds(db_path, 0, 0, 1, 2);
  const auto state = load_sync_state(db_path, "proj-invalid-root");
  REQUIRE(state.has_value());
  REQUIRE_FALSE(state->last_push_at.has_value());
}

TEST_CASE("ProjectSyncWorker skips project without remote URL", "[sync][worker]") {
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
    holder::project::ProjectRepo projects(db);
    holder::model::Project project;
    project.project_id = "proj-no-remote";
    project.name = "Project";
    project.root_path = local_dir.string();
    project.privacy_mode = "plain";
    project.created_at = 1;
    project.updated_at = 1;
    projects.create(project);
  }

  run_worker_with_intervals_for_seconds(db_path, 0, 0, 1, 2);

  const auto state = load_sync_state(db_path, "proj-no-remote");
  REQUIRE_FALSE(state.has_value());
}

TEST_CASE("ProjectSyncWorker run swallows startup and push-cycle exceptions", "[sync][worker]") {
  const auto dir = holder::test::make_temp_dir();
  // Point db_path at a directory so sqlite open fails in both startup and push cycle.
  const auto bad_db_path = dir;

  holder::core::SignalHandler signals;
  holder::sync::ProjectSyncWorker worker(bad_db_path, 0, 0, 1);
  std::exception_ptr thread_error;
  std::thread thread([&worker, &signals, &thread_error]() {
    try {
      worker.run(signals);
    } catch (...) {
      thread_error = std::current_exception();
    }
  });

  std::this_thread::sleep_for(std::chrono::seconds(2));
  std::raise(SIGTERM);
  thread.join();

  REQUIRE(thread_error == nullptr);
}

TEST_CASE(
    "ProjectSyncWorker startup handles pull and metrics failures on corrupted repo",
    "[sync][worker]"
) {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto remote_dir = dir / "remote_repo";
  const auto local_dir = dir / "local_repo";

  holder::git::GitRepo remote_repo;
  remote_repo.open_or_init(remote_dir);
  remote_repo.write_file("cards/a.md", "seed\n");
  remote_repo.stage_path("cards/a.md");
  remote_repo.commit("seed");

  holder::git::GitRepo local_repo;
  local_repo.open_or_init(local_dir);
  local_repo.write_file("cards/a.md", "local\n");
  local_repo.stage_path("cards/a.md");
  local_repo.commit("seed");
  {
    std::ofstream head(local_dir / ".git" / "HEAD");
    REQUIRE(head.good());
    head << "definitely-not-a-valid-head-ref\n";
  }

  {
    auto db = holder::test::open_db_with_schema(db_path);
    holder::project::ProjectRepo projects(db);
    create_project(projects, "proj-startup-postpull", local_dir, remote_dir.string(), "plain");
  }

  run_worker_with_intervals_for_seconds(
      db_path,
      2000000000, // effectively disable push attempts
      3600, // pull interval irrelevant for startup pass
      1,
      2
  );

  const auto state = load_sync_state(db_path, "proj-startup-postpull");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status.has_value());
  REQUIRE(state->last_pull_status.value() == "failed");
}

TEST_CASE("ProjectSyncWorker catches post-pull metrics refresh failure", "[sync][worker]") {
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
    holder::project::ProjectRepo projects(db);
    holder::project::ProjectSyncRepo sync(db);
    create_project(
        projects,
        "proj-post-pull-metrics-fail",
        local_dir,
        (dir / "missing_remote").string(),
        "plain"
    );
    // Force pull attempt in push cycle.
    sync.record_pull_result(
        "proj-post-pull-metrics-fail",
        "succeeded",
        true,
        std::nullopt,
        now_epoch_seconds() - 10000
    );
  }

  SyncWorkerHookGuard guard;
  holder::sync::ProjectSyncWorker::set_fail_post_pull_metrics_for_tests(true);
  run_worker_with_intervals_for_seconds(db_path, 3600, 1, 1, 2);

  const auto state = load_sync_state(db_path, "proj-post-pull-metrics-fail");
  REQUIRE(state.has_value());
  REQUIRE(state->last_pull_status.has_value());
}

TEST_CASE("ProjectSyncWorker catches post-push metrics refresh failure", "[sync][worker]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto remote_dir = dir / "remote_repo";
  const auto local_dir = dir / "local_repo";

  holder::git::GitRepo remote_repo;
  remote_repo.open_or_init(remote_dir);

  holder::git::GitRepo local_repo;
  local_repo.open_or_init(local_dir);
  local_repo.write_file("cards/a.md", "hello");
  local_repo.stage_path("cards/a.md");
  local_repo.commit("seed");

  {
    auto db = holder::test::open_db_with_schema(db_path);
    holder::project::ProjectRepo projects(db);
    holder::project::ProjectSyncRepo sync(db);
    create_project(
        projects,
        "proj-post-push-metrics-fail",
        local_dir,
        remote_dir.string(),
        "plain"
    );
    // Skip pull path; focus on push path.
    sync.record_pull_result(
        "proj-post-push-metrics-fail",
        "succeeded",
        true,
        std::nullopt,
        now_epoch_seconds()
    );
  }

  SyncWorkerHookGuard guard;
  holder::sync::ProjectSyncWorker::set_fail_post_push_metrics_for_tests(true);
  run_worker_with_intervals_for_seconds(db_path, 0, 3600, 1, 2);

  const auto state = load_sync_state(db_path, "proj-post-push-metrics-fail");
  REQUIRE(state.has_value());
  REQUIRE(state->last_push_status.has_value());
}
