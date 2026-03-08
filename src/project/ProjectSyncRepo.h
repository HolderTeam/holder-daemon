#pragma once

#include "model/ProjectSyncState.h"
#include "platform/Db.h"

#include <optional>
#include <string>

namespace holder::project {

class ProjectSyncRepo {
public:
  explicit ProjectSyncRepo(holder::store::Db& db);

  std::optional<holder::model::ProjectSyncState> get(const std::string& project_id) const;

  void record_push_result(const std::string& project_id,
                          const std::string& status,
                          bool success,
                          const std::optional<std::string>& error_message,
                          long long now);
  void record_pull_result(const std::string& project_id,
                          const std::string& status,
                          bool success,
                          const std::optional<std::string>& error_message,
                          long long now);
  void update_activity_counts(const std::string& project_id,
                              int uncommitted_changes_count,
                              int unpushed_commits_count,
                              long long now);
  void remove(const std::string& project_id);

private:
  void ensure_table();
  void upsert(const holder::model::ProjectSyncState& state);

  holder::store::Db& db_;
};

} // namespace holder::project
