#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace holder::api::support {

struct RunEvent {
  std::string name;
  nlohmann::json data;
};

struct RunEventStream {
  std::vector<RunEvent> events;
  bool finished = false;
  long long updated_at = 0;
};

void append_run_event(const std::string& run_id,
                      std::string name,
                      nlohmann::json data,
                      bool finished);
std::optional<RunEventStream> get_run_event_stream(const std::string& run_id);

} // namespace holder::api::support
