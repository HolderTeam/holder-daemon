#pragma once

#include "llm/RunnerTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace holder::llm {

class RunnerClient {
 public:
  virtual ~RunnerClient() = default; // LCOV_EXCL_LINE

  virtual void start_background_probe() = 0;
  virtual RunnerStatus status() const = 0;
  virtual RunnerStatus retry() = 0;
  virtual RunnerPullJob start_pull(const std::string& model) = 0;
  virtual std::optional<RunnerPullJob> get_pull(const std::string& job_id) const = 0;
  virtual std::vector<RunnerPullJob> list_pulls() const = 0;
  virtual bool stream_generate(const std::string& model,
                               const std::string& prompt,
                               const std::string& options_json,
                               const std::function<void(const std::string&)>& on_chunk,
                               std::string* error) = 0;
};

} // namespace holder::llm
