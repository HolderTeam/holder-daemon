#pragma once

#include "model/AiRun.h"
#include "store/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::store {

class AiRunRepo {
 public:
  explicit AiRunRepo(Db& db);

  void create(const model::AiRun& run);
  std::optional<model::AiRun> get(const std::string& run_id) const;
  std::vector<model::AiRun> list_by_thread(const std::string& thread_id) const;
  std::vector<model::AiRun> list_by_project(const std::string& project_id) const;
  void update_status(const std::string& run_id,
                     const std::string& status,
                     const std::optional<std::string>& error,
                     const std::optional<std::string>& message_id,
                     const std::optional<std::string>& chosen_model,
                     const std::optional<std::string>& ranked_json,
                     long long updated_at);

 private:
  Db& db_;
};

} // namespace holder::store
