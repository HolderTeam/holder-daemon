#include "api/support/CloudQuota.h"

#include <sqlite3.h>

#include <cstdlib>
#include <stdexcept>

namespace holder::api::support {

CloudQuotaWindowUsage load_cloud_window_usage(holder::store::Db& db,
                                              const std::string& provider,
                                              const std::string& model_id,
                                              long long since_epoch_seconds) {
  static constexpr const char* SQL =
      "SELECT COUNT(*), COALESCE(SUM(total_tokens), 0) "
      "FROM ai_cloud_usage_events "
      "WHERE provider = ? AND model_id = ? AND created_at >= ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare cloud usage query failed");
  }
  sqlite3_bind_text(stmt, 1, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, model_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, since_epoch_seconds);

  CloudQuotaWindowUsage usage;
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    usage.requests = sqlite3_column_int64(stmt, 0);
    usage.tokens = sqlite3_column_int64(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return usage;
}

void record_cloud_usage_event(holder::store::Db& db,
                              const std::string& provider,
                              const std::string& model_id,
                              long long prompt_tokens,
                              long long response_tokens,
                              long long created_at,
                              const std::string& event_id_seed) {
  static constexpr const char* SQL =
      "INSERT INTO ai_cloud_usage_events("
      "event_id, provider, model_id, prompt_tokens, response_tokens, total_tokens, created_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?);";

  const std::string event_id =
      "cloud-" + event_id_seed + "-" + std::to_string(created_at) + "-" +
      std::to_string(static_cast<unsigned long long>(std::rand()));
  const long long total_tokens = prompt_tokens + response_tokens;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare cloud usage insert failed");
  }
  sqlite3_bind_text(stmt, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, model_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, prompt_tokens);
  sqlite3_bind_int64(stmt, 5, response_tokens);
  sqlite3_bind_int64(stmt, 6, total_tokens);
  sqlite3_bind_int64(stmt, 7, created_at);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error("insert cloud usage event failed");
  }
}

} // namespace holder::api::support
