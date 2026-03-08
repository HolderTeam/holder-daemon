#pragma once

#include "model/AiRouterConfig.h"
#include "store/Db.h"

#include <optional>
#include <string>

namespace holder::ai {

class AiRouterConfigRepo {
 public:
  explicit AiRouterConfigRepo(holder::store::Db& db);

  std::optional<holder::model::AiRouterConfig> get_global() const;
  std::optional<holder::model::AiRouterConfig> get_for_project(const std::string& project_id) const;

  void set_global(const std::string& router_model, long long updated_at);
  void set_for_project(const std::string& project_id,
                       const std::string& router_model,
                       long long updated_at);

  void clear_global();
  void clear_for_project(const std::string& project_id);

 private:
  holder::store::Db& db_;
};

} // namespace holder::ai
