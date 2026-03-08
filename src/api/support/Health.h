#pragma once

#include "platform/Db.h"

#include <chrono>

#include <nlohmann/json.hpp>

namespace holder::api::support {

nlohmann::json build_health_data(holder::platform::Db& db,
                                 std::chrono::steady_clock::time_point started_at);

} // namespace holder::api::support
