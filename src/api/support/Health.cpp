#include "api/support/Health.h"

#include "platform/ServerInfo.h"

namespace holder::api::support {
nlohmann::json build_health_data(holder::store::Db& db,
                                 std::chrono::steady_clock::time_point started_at) {
  bool db_ok = true;
  try {
    db.exec("SELECT 1;");
  } catch (...) {
    db_ok = false;
  }

  const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started_at)
                             .count();

  nlohmann::json data;
  data["db_ok"] = db_ok;
  data["uptime_ms"] = uptime_ms;
  data["api_version"] = "0.1";
  data["server_version"] = CARD_SERVER_VERSION;
  data["pid"] = holder::core::current_pid();
  data["privacy"] = {
      {"backend", "libsodium_xchacha20poly1305_ietf"},
      {"project_mode_supported", true},
  };

  return data;
}

} // namespace holder::api::support
