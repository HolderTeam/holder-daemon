#pragma once

#include "llm/RunnerTypes.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

namespace holder::llm {

class LocalModelRunner {
 public:
  using StreamGenerateOverride =
      std::function<bool(const std::string&,
                         const std::string&,
                         const std::string&,
                         const std::function<void(const std::string&)>&,
                         std::string*)>;

  LocalModelRunner();
  LocalModelRunner(std::string host,
                   std::string port,
                   std::string exec_path,
                   bool allow_spawn);
  ~LocalModelRunner();

  void start_background_probe();
  RunnerStatus status() const;
  RunnerStatus retry();
  void stop();

  using PullProgress = RunnerPullProgress;
  using PullJob = RunnerPullJob;

  PullJob start_pull(const std::string& model);
  std::optional<PullJob> get_pull(const std::string& job_id) const;
  std::vector<PullJob> list_pulls() const;
  bool stream_generate(const std::string& model,
                       const std::string& prompt,
                       const std::string& options_json,
                       const std::function<void(const std::string&)>& on_chunk,
                       std::string* error);
  void set_fake_mode(bool enabled);
  void set_status_override_for_tests(const std::optional<RunnerStatus>& status);
  void set_stream_generate_override_for_tests(StreamGenerateOverride override_fn);
  void clear_overrides_for_tests();

 private:
  void probe(bool allow_spawn);
  bool http_get_json(const std::string& target, std::string* out, std::string* error);
  bool try_spawn(std::string* error);
  void run_pull(const std::string& job_id, const std::string& model);
  void maybe_complete_fake_pulls_locked() const;
  static std::string generate_job_id();

  std::string host_;
  std::string port_;
  std::string exec_path_;
  bool allow_spawn_ = true;

  std::atomic<bool> spawn_attempted_{false};
  std::atomic<bool> background_started_{false};
  bool fake_mode_ = false;

  mutable std::mutex mu_;
  RunnerStatus status_;
  std::optional<RunnerStatus> status_override_for_tests_;
  StreamGenerateOverride stream_generate_override_for_tests_;
  struct RunnerProcess;
  std::unique_ptr<RunnerProcess> process_;

  mutable std::mutex pulls_mu_;
  mutable std::unordered_map<std::string, PullJob> pulls_;
};

} // namespace holder::llm
