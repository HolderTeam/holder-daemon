#pragma once

#include <optional>
#include <string>

namespace holder::model {

struct AiRouterConfig {
  std::string scope; // global | project
  std::optional<std::string> project_id;
  std::string router_model;
  long long updated_at = 0;
};

} // namespace holder::model
