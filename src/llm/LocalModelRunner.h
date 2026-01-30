#pragma once

#include <atomic>
#include <mutex>
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

  void start_background_probe();
  RunnerStatus status() const;
  RunnerStatus retry();

 private:
  void probe(bool allow_spawn);
  bool http_get_json(const std::string& target, std::string* out, std::string* error);
  bool try_spawn(std::string* error);

  std::string host_;
  std::string port_;
  std::string exec_path_;

  std::atomic<bool> spawn_attempted_{false};
  std::atomic<bool> background_started_{false};

  mutable std::mutex mu_;
  RunnerStatus status_;
};

} // namespace holder::llm
