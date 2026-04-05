#include "llm/ExecutorRunnerClient.h"

namespace holder::llm {

ExecutorRunnerClient::ExecutorRunnerClient(std::unique_ptr<RunnerClient> inner,
                                           const holder::core::SerialExecutor& executor)
    : owned_inner_(std::move(inner)),
      inner_(owned_inner_.get()),
      executor_(executor) {}

ExecutorRunnerClient::ExecutorRunnerClient(RunnerClient& inner,
                                           const holder::core::SerialExecutor& executor)
    : inner_(&inner),
      executor_(executor) {}

ExecutorRunnerClient::~ExecutorRunnerClient() = default;

void ExecutorRunnerClient::start_background_probe() {
  if (inner_ == nullptr) {
    return;
  }
  executor_.submit([this]() { inner_->start_background_probe(); });
}

RunnerStatus ExecutorRunnerClient::status() const {
  return inner_ != nullptr ? executor_.call([&]() { return inner_->status(); }) : RunnerStatus{};
}

RunnerStatus ExecutorRunnerClient::retry() {
  return inner_ != nullptr ? executor_.call([&]() { return inner_->retry(); }) : RunnerStatus{};
}

RunnerPullJob ExecutorRunnerClient::start_pull(const std::string& model) {
  return inner_ != nullptr ? executor_.call([&]() { return inner_->start_pull(model); })
                           : RunnerPullJob{};
}

std::optional<RunnerPullJob> ExecutorRunnerClient::get_pull(const std::string& job_id) const {
  return inner_ != nullptr ? executor_.call([&]() { return inner_->get_pull(job_id); })
                           : std::nullopt;
}

std::vector<RunnerPullJob> ExecutorRunnerClient::list_pulls() const {
  return inner_ != nullptr ? executor_.call([&]() { return inner_->list_pulls(); })
                           : std::vector<RunnerPullJob>{};
}

bool ExecutorRunnerClient::stream_generate(const std::string& model,
                                           const std::string& prompt,
                                           const std::string& options_json,
                                           const std::function<void(const std::string&)>& on_chunk,
                                           std::string* error) {
  return inner_ != nullptr &&
         executor_.call(
             [&]() { return inner_->stream_generate(model, prompt, options_json, on_chunk, error); });
}

} // namespace holder::llm
