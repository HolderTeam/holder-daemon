#pragma once

#include "core/SerialExecutor.h"
#include "llm/RunnerClient.h"

namespace holder::llm {

class ExecutorRunnerClient final : public RunnerClient {
 public:
  ExecutorRunnerClient(
      std::unique_ptr<RunnerClient> inner,
      const holder::core::SerialExecutor& executor
  );
  ExecutorRunnerClient(RunnerClient& inner, const holder::core::SerialExecutor& executor);
  ~ExecutorRunnerClient() override;

  void start_background_probe() override;
  RunnerStatus status() const override;
  RunnerStatus retry() override;
  RunnerPullJob start_pull(const std::string& model) override;
  std::optional<RunnerPullJob> get_pull(const std::string& job_id) const override;
  std::vector<RunnerPullJob> list_pulls() const override;
  bool stream_generate(
      const std::string& model,
      const std::string& prompt,
      const std::string& options_json,
      const std::function<void(const std::string&)>& on_chunk,
      std::string* error
  ) override;

 private:
  std::unique_ptr<RunnerClient> owned_inner_;
  RunnerClient* inner_ = nullptr;
  const holder::core::SerialExecutor& executor_;
};

} // namespace holder::llm
