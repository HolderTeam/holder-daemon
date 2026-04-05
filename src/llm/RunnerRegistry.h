#pragma once

#include "llm/RunnerClient.h"
#include "model/AiRunner.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace holder::platform {
class Db;
}

namespace holder::llm {

class RunnerRegistry {
 public:
  static constexpr const char* kAutoLocalRunnerId = "auto-local";

  explicit RunnerRegistry(holder::platform::Db* db = nullptr,
                          RunnerClient* auto_local_client = nullptr);

  std::vector<holder::model::AiRunner> list_runners() const;
  std::optional<holder::model::AiRunner> get_runner(const std::string& runner_id) const;
  RunnerClient* get_client(const std::string& runner_id) const;

 private:
  void load_manual_clients();

  holder::platform::Db* db_ = nullptr;
  RunnerClient* auto_local_client_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<RunnerClient>> manual_clients_;
};

} // namespace holder::llm
