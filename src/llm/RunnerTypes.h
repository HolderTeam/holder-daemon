#pragma once

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

struct RunnerPullProgress {
  long long completed = 0;
  long long total = 0;
  double percent = 0.0;
  std::string stage;
};

struct RunnerPullJob {
  std::string job_id;
  std::string model;
  std::string status;
  RunnerPullProgress progress;
  long long updated_at = 0;
  std::string error;
};

} // namespace holder::llm
