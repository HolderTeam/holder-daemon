#include "sync/ProjectSyncWorker.h"

#include "git/GitOps.h"
#include "git/RepoSyncMetrics.h"
#include "privacy/ProjectPrivacy.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"
#include "store/ProjectSyncRepo.h"
#include "sync/ProjectSyncPolicy.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <thread>

namespace holder::sync {
namespace {

bool is_push_success(holder::git::PushStatus status) {
  return status == holder::git::PushStatus::Pushed ||
         status == holder::git::PushStatus::UpToDate;
}

} // namespace

ProjectSyncWorker::ProjectSyncWorker(std::filesystem::path db_path,
                                     int push_interval_seconds,
                                     int poll_interval_seconds)
    : db_path_(std::move(db_path)),
      push_interval_seconds_(push_interval_seconds),
      poll_interval_seconds_(poll_interval_seconds) {}

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

long long ProjectSyncWorker::now_epoch_seconds() const {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void ProjectSyncWorker::run_startup_pull_pass() {
  holder::store::Db db;
  db.open(db_path_);
  holder::store::ProjectRepo projects(db);
  holder::store::ProjectSyncRepo sync(db);
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
      sync.update_activity_counts(project.project_id,
                                  metrics.uncommitted_changes_count,
                                  metrics.unpushed_commits_count,
                                  now);
    } catch (const std::exception& ex) {
      spdlog::warn("sync worker startup metrics refresh failed for {}: {}",
                   project.project_id,
                   ex.what());
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
      sync.record_pull_result(project.project_id,
                              "failed",
                              false,
                              std::optional<std::string>(ex.what()),
                              now);
    }
    try {
      const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
      sync.update_activity_counts(project.project_id,
                                  metrics.uncommitted_changes_count,
                                  metrics.unpushed_commits_count,
                                  now);
    } catch (const std::exception& ex) {
      spdlog::warn("sync worker startup post-pull metrics refresh failed for {}: {}",
                   project.project_id,
                   ex.what());
    }
  }
}

void ProjectSyncWorker::run_push_cycle() {
  holder::store::Db db;
  db.open(db_path_);
  holder::store::ProjectRepo projects(db);
  holder::store::ProjectSyncRepo sync(db);
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
      sync.update_activity_counts(project.project_id,
                                  metrics.uncommitted_changes_count,
                                  metrics.unpushed_commits_count,
                                  now);
    } catch (const std::exception& ex) {
      spdlog::warn("sync worker metrics refresh failed for {}: {}",
                   project.project_id,
                   ex.what());
      continue;
    }

    const auto state = sync.get(project.project_id);
    if (!should_attempt_push(
            {.last_push_at = state.has_value() ? state->last_push_at : std::optional<long long>{},
             .next_retry_at = state.has_value() ? state->next_retry_at : std::optional<long long>{},
             .now = now,
             .push_interval_seconds = push_interval_seconds_})) {
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
          now);
    } catch (const std::exception& ex) {
      sync.record_push_result(project.project_id,
                              holder::git::push_status_name(holder::git::PushStatus::UnknownError),
                              false,
                              std::optional<std::string>(ex.what()),
                              now);
    }
    try {
      const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
      sync.update_activity_counts(project.project_id,
                                  metrics.uncommitted_changes_count,
                                  metrics.unpushed_commits_count,
                                  now);
    } catch (const std::exception& ex) {
      spdlog::warn("sync worker post-push metrics refresh failed for {}: {}",
                   project.project_id,
                   ex.what());
    }
  }
}

} // namespace holder::sync
