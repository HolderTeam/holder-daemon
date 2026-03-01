#pragma once

#include <optional>

namespace holder::sync {

struct PushDecisionInput {
  std::optional<long long> last_push_at;
  std::optional<long long> next_retry_at;
  long long now = 0;
  int push_interval_seconds = 1200;
};

bool should_attempt_push(const PushDecisionInput& input);

} // namespace holder::sync

