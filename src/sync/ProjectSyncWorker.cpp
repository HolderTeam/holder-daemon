#include "sync/ProjectSyncWorker.h"

#include "git/GitOps.h"
#include "git/RepoSyncMetrics.h"
#include "platform/Db.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectRepo.h"
#include "project/ProjectSyncRepo.h"
#include "sync/ProjectSyncPolicy.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <exception>
#include <thread>

namespace holder::sync {
namespace {

std::atomic<bool> g_fail_post_pull_metrics_for_tests{false};
std::atomic<bool> g_fail_post_push_metrics_for_tests{false};

bool is_push_success(holder::git::PushStatus status) {
  return status == holder::git::PushStatus::Pushed || status == holder::git::PushStatus::UpToDate;
}

} // namespace

ProjectSyncWorker::ProjectSyncWorker(
    std::filesystem::path db_path,
    ProjectSyncWorkerIntervals intervals
)
    : db_path_(std::move(db_path)),
      push_interval_seconds_(intervals.push_interval_seconds),
      pull_interval_seconds_(intervals.pull_interval_seconds),
      poll_interval_seconds_(intervals.poll_interval_seconds) {}

void ProjectSyncWorker::run(const holder::core::SignalHandler& signals) {
  try {
    run_startup_pull_pass();
  } catch (const std::exception& ex) {
    spdlog::warn("sync worker startup pull pass failed: {}", ex.what());
  }

  while (!signals.is_requested()) {
    try {
      run_push_cycle();
    } catch (const std::exception& ex) {
      spdlog::warn("sync worker push cycle failed: {}", ex.what());
    }

    int slept = 0;
    while (!signals.is_requested() && slept < poll_interval_seconds_) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      slept += 1;
    }
  }
}

void ProjectSyncWorker::set_fail_post_pull_metrics_for_tests(bool enabled) {
  g_fail_post_pull_metrics_for_tests.store(enabled, std::memory_order_relaxed);
}

void ProjectSyncWorker::set_fail_post_push_metrics_for_tests(bool enabled) {
  g_fail_post_push_metrics_for_tests.store(enabled, std::memory_order_relaxed);
}

long long ProjectSyncWorker::now_epoch_seconds() const {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch()
  )
      .count();
}

void ProjectSyncWorker::run_startup_pull_pass() {
  holder::platform::Db db;
  db.open(db_path_);
  holder::project::ProjectRepo projects(db);
  holder::project::ProjectSyncRepo sync(db);
  holder::git::RealGitOps git;

  const auto now = now_epoch_seconds();
  for (const auto& project : projects.list()) {
    if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
      continue;
    }
    try {
      git.open_or_init(project.root_path);
      git.set_remote("origin", project.git_remote_url.value());
      const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
      sync.update_activity_counts(
          project.project_id,
          {.uncommitted_changes_count = metrics.uncommitted_changes_count,
           .unpushed_commits_count = metrics.unpushed_commits_count,
           .updated_at = now}
      );
    } catch (const std::exception& ex) {
      spdlog::warn(
          "sync worker startup metrics refresh failed for {}: {}",
          project.project_id,
          ex.what()
      );
    }
    const auto current = sync.get(project.project_id);
    if (current.has_value() && current->last_pull_at.has_value()) {
      continue;
    }
    try {
      git.open_or_init(project.root_path);
      git.set_remote("origin", project.git_remote_url.value());
      git.pull_remote_ff_only("origin");
      sync.record_pull_result(project.project_id, "succeeded", true, std::nullopt, now);
    } catch (const std::exception& ex) {
      sync.record_pull_result(
          project.project_id,
          "failed",
          false,
          std::optional<std::string>(ex.what()),
          now
      );
    }
    try {
      const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
      sync.update_activity_counts(
          project.project_id,
          {.uncommitted_changes_count = metrics.uncommitted_changes_count,
           .unpushed_commits_count = metrics.unpushed_commits_count,
           .updated_at = now}
      );
    } catch (const std::exception& ex) {
      spdlog::warn(
          "sync worker startup post-pull metrics refresh failed for {}: {}",
          project.project_id,
          ex.what()
      );
    }
  }
}

void ProjectSyncWorker::run_push_cycle() {
  holder::platform::Db db;
  db.open(db_path_);
  holder::project::ProjectRepo projects(db);
  holder::project::ProjectSyncRepo sync(db);
  holder::git::RealGitOps git;

  const auto now = now_epoch_seconds();
  for (const auto& project : projects.list()) {
    if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
      continue;
    }
    try {
      git.open_or_init(project.root_path);
      git.set_remote("origin", project.git_remote_url.value());
      const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
      sync.update_activity_counts(
          project.project_id,
          {.uncommitted_changes_count = metrics.uncommitted_changes_count,
           .unpushed_commits_count = metrics.unpushed_commits_count,
           .updated_at = now}
      );
    } catch (const std::exception& ex) {
      spdlog::warn("sync worker metrics refresh failed for {}: {}", project.project_id, ex.what());
      continue;
    }

    const auto state = sync.get(project.project_id);
    if (should_attempt_pull(
            {.last_pull_at = state.has_value() ? state->last_pull_at : std::optional<long long>{},
             .next_pull_retry_at = state.has_value() ? state->next_pull_retry_at
                                                     : std::optional<long long>{},
             .now = now,
             .pull_interval_seconds = pull_interval_seconds_}
        )) {
      try {
        git.open_or_init(project.root_path);
        git.set_remote("origin", project.git_remote_url.value());
        git.pull_remote_ff_only("origin");
        sync.record_pull_result(project.project_id, "succeeded", true, std::nullopt, now);
      } catch (const std::exception& ex) {
        sync.record_pull_result(
            project.project_id,
            "failed",
            false,
            std::optional<std::string>(ex.what()),
            now
        );
      }

      try {
        if (g_fail_post_pull_metrics_for_tests.load(std::memory_order_relaxed)) {
          throw std::runtime_error("forced post-pull metrics failure for tests");
        }
        const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
        sync.update_activity_counts(
            project.project_id,
            {.uncommitted_changes_count = metrics.uncommitted_changes_count,
             .unpushed_commits_count = metrics.unpushed_commits_count,
             .updated_at = now}
        );
      } catch (const std::exception& ex) {
        spdlog::warn(
            "sync worker post-pull metrics refresh failed for {}: {}",
            project.project_id,
            ex.what()
        );
      }
    }

    if (!should_attempt_push(
            {.last_push_at = state.has_value() ? state->last_push_at : std::optional<long long>{},
             .next_retry_at = state.has_value() ? state->next_retry_at : std::optional<long long>{},
             .now = now,
             .push_interval_seconds = push_interval_seconds_}
        )) {
      continue;
    }

    try {
      git.open_or_init(project.root_path);
      git.set_remote("origin", project.git_remote_url.value());
      if (project.privacy_mode == "encrypted_git") {
        holder::privacy::assert_encryption_push_safe(project.root_path);
      }
      const auto result = git.push_branch("origin", "", true);
      sync.record_push_result(
          project.project_id,
          holder::git::push_status_name(result.status),
          is_push_success(result.status),
          result.error_message.empty() ? std::optional<std::string>()
                                       : std::optional<std::string>(result.error_message),
          now
      );
    } catch (const std::exception& ex) {
      sync.record_push_result(
          project.project_id,
          holder::git::push_status_name(holder::git::PushStatus::UnknownError),
          false,
          std::optional<std::string>(ex.what()),
          now
      );
    }
    try {
      if (g_fail_post_push_metrics_for_tests.load(std::memory_order_relaxed)) {
        throw std::runtime_error("forced post-push metrics failure for tests");
      }
      const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
      sync.update_activity_counts(
          project.project_id,
          {.uncommitted_changes_count = metrics.uncommitted_changes_count,
           .unpushed_commits_count = metrics.unpushed_commits_count,
           .updated_at = now}
      );
    } catch (const std::exception& ex) {
      spdlog::warn(
          "sync worker post-push metrics refresh failed for {}: {}",
          project.project_id,
          ex.what()
      );
    }
  }
}

} // namespace holder::sync
