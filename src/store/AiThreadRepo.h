#pragma once

#include "model/AiThread.h"
#include "store/Db.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::store {

class AiThreadRepo {
public:
  explicit AiThreadRepo(Db& db);

  void create(const holder::model::AiThread& thread);
  std::optional<holder::model::AiThread> get(const std::string& thread_id) const;
  std::vector<holder::model::AiThread> list(const std::string& project_id) const;

  void update_title(const std::string& thread_id, const std::string& title, long long updated_at);
  void touch_updated(const std::string& thread_id, long long updated_at);
  void remove(const std::string& thread_id);

private:
  Db& db_;
};

} // namespace holder::store
