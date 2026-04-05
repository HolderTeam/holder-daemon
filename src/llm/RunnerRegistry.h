#pragma once

#include <optional>
#include <string>
#include <vector>

namespace holder::llm {

class LocalModelRunner;

struct RunnerRecord {
  std::string runner_id;
  std::string name;
  std::string kind;
  std::optional<std::string> base_url;
  std::string source;
  bool enabled = true;
};

class RunnerRegistry {
 public:
  static constexpr const char* kAutoLocalRunnerId = "auto-local";

  explicit RunnerRegistry(LocalModelRunner* auto_local_runner = nullptr);

  std::vector<RunnerRecord> list_runners() const;
  std::optional<RunnerRecord> get_runner(const std::string& runner_id) const;
  LocalModelRunner* get_auto_local_runner() const;
  LocalModelRunner* get_runner_for_compat(const std::string& runner_id) const;

 private:
  LocalModelRunner* auto_local_runner_ = nullptr;
};

} // namespace holder::llm
