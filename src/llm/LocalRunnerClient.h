#pragma once

#include "llm/RunnerClient.h"

#include <memory>

namespace holder::llm {

class LocalModelRunner;

class LocalRunnerClient final : public RunnerClient {
 public:
  explicit LocalRunnerClient(LocalModelRunner* runner);
  explicit LocalRunnerClient(std::unique_ptr<LocalModelRunner> runner);
  ~LocalRunnerClient() override;

  void start_background_probe() override;
  RunnerStatus status() const override;
  RunnerStatus retry() override;
  RunnerPullJob start_pull(const std::string& model) override;
  std::optional<RunnerPullJob> get_pull(const std::string& job_id) const override;
  std::vector<RunnerPullJob> list_pulls() const override;
  bool stream_generate(const std::string& model,
                       const std::string& prompt,
                       const std::string& options_json,
                       const std::function<void(const std::string&)>& on_chunk,
                       std::string* error) override;

 private:
  std::unique_ptr<LocalModelRunner> owned_runner_;
  LocalModelRunner* runner_ = nullptr;
};

} // namespace holder::llm
