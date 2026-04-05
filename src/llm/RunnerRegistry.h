#pragma once

#include "model/AiRunner.h"

#include <optional>
#include <string>
#include <vector>

namespace holder::platform {
class Db;
}

namespace holder::llm {
class LocalModelRunner;

class RunnerRegistry {
 public:
  static constexpr const char* kAutoLocalRunnerId = "auto-local";

  explicit RunnerRegistry(holder::platform::Db* db = nullptr,
                          LocalModelRunner* auto_local_runner = nullptr);

  std::vector<holder::model::AiRunner> list_runners() const;
  std::optional<holder::model::AiRunner> get_runner(const std::string& runner_id) const;
  LocalModelRunner* get_auto_local_runner() const;

 private:
  holder::platform::Db* db_ = nullptr;
  LocalModelRunner* auto_local_runner_ = nullptr;
};

} // namespace holder::llm
