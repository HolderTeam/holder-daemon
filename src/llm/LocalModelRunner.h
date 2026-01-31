#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

namespace holder::llm {

struct LocalModel {
  std::string name;
  std::string digest;
  long long size = 0;
  std::string modified_at;
};

struct RunnerStatus {
  bool available = false;
  bool spawn_attempted = false;
  long long last_checked = 0;
  std::string version;
  std::string error;
  std::vector<LocalModel> models;
};

class LocalModelRunner {
 public:
  LocalModelRunner();
  ~LocalModelRunner();

  void start_background_probe();
  RunnerStatus status() const;
  RunnerStatus retry();
  void stop();

  struct PullProgress {
    long long completed = 0;
    long long total = 0;
    double percent = 0.0;
    std::string stage;
  };

  struct PullJob {
    std::string job_id;
    std::string model;
    std::string status;
    PullProgress progress;
    long long updated_at = 0;
    std::string error;
  };

  PullJob start_pull(const std::string& model);
  std::optional<PullJob> get_pull(const std::string& job_id) const;
  bool stream_generate(const std::string& model,
                       const std::string& prompt,
                       const std::string& options_json,
                       const std::function<void(const std::string&)>& on_chunk,
                       std::string* error);

 private:
  void probe(bool allow_spawn);
  bool http_get_json(const std::string& target, std::string* out, std::string* error);
  bool try_spawn(std::string* error);
  void run_pull(const std::string& job_id, const std::string& model);
  static std::string generate_job_id();

  std::string host_;
  std::string port_;
  std::string exec_path_;

  std::atomic<bool> spawn_attempted_{false};
  std::atomic<bool> background_started_{false};

  mutable std::mutex mu_;
  RunnerStatus status_;
  struct RunnerProcess;
  std::unique_ptr<RunnerProcess> process_;

  mutable std::mutex pulls_mu_;
  std::unordered_map<std::string, PullJob> pulls_;
};

} // namespace holder::llm
