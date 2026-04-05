#include "llm/RunnerRegistry.h"

#include "llm/LocalModelRunner.h"

namespace holder::llm {

RunnerRegistry::RunnerRegistry(LocalModelRunner* auto_local_runner)
    : auto_local_runner_(auto_local_runner) {}

std::vector<RunnerRecord> RunnerRegistry::list_runners() const {
  std::vector<RunnerRecord> out;
  out.push_back(RunnerRecord{
      .runner_id = kAutoLocalRunnerId,
      .name = "Local Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://127.0.0.1:11434"),
      .source = "auto_local",
      .enabled = true,
  });
  return out;
}

std::optional<RunnerRecord> RunnerRegistry::get_runner(const std::string& runner_id) const {
  if (runner_id == kAutoLocalRunnerId) {
    return list_runners().front();
  }
  return std::nullopt;
}

LocalModelRunner* RunnerRegistry::get_auto_local_runner() const {
  return auto_local_runner_;
}

LocalModelRunner* RunnerRegistry::get_runner_for_compat(const std::string& runner_id) const {
  if (runner_id == kAutoLocalRunnerId) {
    return auto_local_runner_;
  }
  return nullptr;
}

} // namespace holder::llm
