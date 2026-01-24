#pragma once

#include "model/Project.h"
#include "store/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::store {

class ProjectRepo {
public:
  explicit ProjectRepo(Db& db);

  void create(const holder::model::Project& project);
  std::optional<holder::model::Project> get(const std::string& project_id) const;
  std::vector<holder::model::Project> list() const;

  void update_name(const std::string& project_id, const std::string& name, long long updated_at);
  void update_root_path(const std::string& project_id, const std::string& root_path, long long updated_at);
  void update_git_remote(const std::string& project_id,
                         const std::optional<std::string>& git_remote_url,
                         long long updated_at);
  void update_git_provider(const std::string& project_id,
                           const std::optional<std::string>& git_provider,
                           long long updated_at);
  void touch_updated(const std::string& project_id, long long updated_at);
  void remove(const std::string& project_id);

private:
  Db& db_;
};

} // namespace holder::store
