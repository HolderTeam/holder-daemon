#pragma once

#include <chrono>

namespace holder::api::support {

inline long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace holder::api::support
