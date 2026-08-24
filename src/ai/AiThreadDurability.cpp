#include "ai/AiThreadDurability.h"

#include "ai/AiThreadManifest.h"
#include "ai/AiThreadRepo.h"
#include "git/GitOps.h"
#include "project/ProjectRepo.h"

#include <filesystem>
#include <stdexcept>

namespace holder::ai {
namespace {

holder::model::Project require_project(
    holder::platform::Db& db,
    const std::string& project_id
) {
  const auto project = holder::project::ProjectRepo(db).get(project_id);
  if (!project.has_value()) throw std::runtime_error("project not found for AI thread");
  return *project;
}

} // namespace

void persist_ai_thread(
    holder::platform::Db& db,
    const holder::model::AiThread& thread,
    const std::string& commit_message
) {
  const auto project = require_project(db, thread.project_id);
  holder::git::RealGitOps git;
  holder::ai::write_ai_thread_manifest(git, project, thread);
  git.commit(commit_message);
}

void remove_ai_thread_manifest(holder::platform::Db& db, const holder::model::AiThread& thread) {
  const auto project = require_project(db, thread.project_id);
  holder::git::RealGitOps git;
  git.open_or_init(project.root_path);
  const auto rel_path = holder::ai::ai_thread_manifest_rel_path(thread.thread_id);
  const auto full_path = std::filesystem::path(project.root_path) / rel_path;
  if (!std::filesystem::exists(full_path)) return;
  std::filesystem::remove(full_path);
  git.remove_path(rel_path);
  git.commit("Remove AI thread metadata");
}

std::size_t backfill_ai_thread_manifests(holder::platform::Db& db) {
  holder::project::ProjectRepo projects(db);
  holder::ai::AiThreadRepo threads(db);
  std::size_t written = 0;
  for (const auto& project : projects.list()) {
    holder::git::RealGitOps git;
    bool changed = false;
    for (const auto& thread : threads.list(project.project_id)) {
      const auto rel_path = holder::ai::ai_thread_manifest_rel_path(thread.thread_id);
      if (std::filesystem::is_regular_file(std::filesystem::path(project.root_path) / rel_path)) {
        continue;
      }
      holder::ai::write_ai_thread_manifest(git, project, thread);
      changed = true;
      ++written;
    }
    if (changed) git.commit("Add durable AI thread metadata");
  }
  return written;
}

} // namespace holder::ai
