#include "llm/RunnerRegistry.h"

#include "ai/AiRunnerRepo.h"
#include "llm/LocalModelRunner.h"

namespace holder::llm {
namespace {

holder::model::AiRunner auto_local_runner_record() {
  return holder::model::AiRunner{
      .runner_id = RunnerRegistry::kAutoLocalRunnerId,
      .name = "Local Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://127.0.0.1:11434"),
      .source = "auto_local",
      .enabled = true,
      .created_at = 0,
      .updated_at = 0,
  };
}

} // namespace

RunnerRegistry::RunnerRegistry(holder::platform::Db* db, LocalModelRunner* auto_local_runner)
    : db_(db), auto_local_runner_(auto_local_runner) {}

std::vector<holder::model::AiRunner> RunnerRegistry::list_runners() const {
  std::vector<holder::model::AiRunner> out;
  out.push_back(auto_local_runner_record());
  if (db_ != nullptr) {
    holder::ai::AiRunnerRepo repo(*db_);
    auto manual = repo.list();
    out.insert(out.end(), manual.begin(), manual.end());
  }
  return out;
}

std::optional<holder::model::AiRunner> RunnerRegistry::get_runner(const std::string& runner_id) const {
  if (runner_id == kAutoLocalRunnerId) {
    return auto_local_runner_record();
  }
  if (db_ != nullptr) {
    holder::ai::AiRunnerRepo repo(*db_);
    return repo.get(runner_id);
  }
  return std::nullopt;
}

LocalModelRunner* RunnerRegistry::get_auto_local_runner() const {
  return auto_local_runner_;
}

} // namespace holder::llm
