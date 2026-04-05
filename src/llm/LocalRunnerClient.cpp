#include "llm/LocalRunnerClient.h"

#include "llm/LocalModelRunner.h"

namespace holder::llm {

LocalRunnerClient::LocalRunnerClient(LocalModelRunner* runner) : runner_(runner) {}

void LocalRunnerClient::start_background_probe() {
  if (runner_ != nullptr) {
    runner_->start_background_probe();
  }
}

RunnerStatus LocalRunnerClient::status() const {
  return runner_ != nullptr ? runner_->status() : RunnerStatus{};
}

RunnerStatus LocalRunnerClient::retry() {
  return runner_ != nullptr ? runner_->retry() : RunnerStatus{};
}

RunnerPullJob LocalRunnerClient::start_pull(const std::string& model) {
  return runner_ != nullptr ? runner_->start_pull(model) : RunnerPullJob{};
}

std::optional<RunnerPullJob> LocalRunnerClient::get_pull(const std::string& job_id) const {
  return runner_ != nullptr ? runner_->get_pull(job_id) : std::nullopt;
}

std::vector<RunnerPullJob> LocalRunnerClient::list_pulls() const {
  return runner_ != nullptr ? runner_->list_pulls() : std::vector<RunnerPullJob>{};
}

bool LocalRunnerClient::stream_generate(const std::string& model,
                                        const std::string& prompt,
                                        const std::string& options_json,
                                        const std::function<void(const std::string&)>& on_chunk,
                                        std::string* error) {
  return runner_ != nullptr &&
         runner_->stream_generate(model, prompt, options_json, on_chunk, error);
}

} // namespace holder::llm
