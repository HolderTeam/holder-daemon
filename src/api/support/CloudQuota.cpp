#include "api/support/CloudQuota.h"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <process.h>
#endif

namespace holder::api::support {
namespace {

std::mutex usage_ledger_mutex;
std::optional<std::filesystem::path> usage_ledger_path;

// A fixed ".tmp" name would let two concurrent writers race on the same temp file
// before either atomic rename happens, corrupting it. Make each writer's temp file
// unique.
std::string unique_temp_suffix() {
  static std::atomic<unsigned long long> counter{0};
#ifdef _WIN32
  const auto pid = static_cast<unsigned long>(::_getpid());
#else
  const auto pid = static_cast<unsigned long>(::getpid());
#endif
  return "." + std::to_string(pid) + "." + std::to_string(counter.fetch_add(1));
}

void restrict_file(const std::filesystem::path& path) {
#ifndef _WIN32
  if (::chmod(path.c_str(), S_IRUSR | S_IWUSR) != 0) {
    throw std::runtime_error("failed to restrict cloud usage ledger permissions");
  }
#else
  (void)path;
#endif
}

nlohmann::json load_ledger(const std::filesystem::path& path) {
  if (!std::filesystem::exists(path)) {
    return {{"version", 1}, {"events", nlohmann::json::array()}};
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("failed to open cloud usage ledger");
  auto body = nlohmann::json::parse(in);
  if (body.value("version", 0) != 1 || !body.contains("events") ||
      !body.at("events").is_array()) {
    throw std::runtime_error("unsupported cloud usage ledger format");
  }
  return body;
}

void write_ledger(const std::filesystem::path& path, const nlohmann::json& body) {
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path;
  temporary += ".tmp" + unique_temp_suffix();
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("failed to write cloud usage ledger");
    out << body.dump(2) << '\n';
    out.flush();
    if (!out) throw std::runtime_error("failed to flush cloud usage ledger");
  }
  restrict_file(temporary);
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
#ifdef _WIN32
  if (ec && std::filesystem::exists(path)) {
    std::filesystem::remove(path, ec);
    if (!ec) std::filesystem::rename(temporary, path, ec);
  }
#endif
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("failed to replace cloud usage ledger: " + ec.message());
  }
  restrict_file(path);
}

void insert_event(holder::platform::Db& db, const nlohmann::json& event) {
  static constexpr const char* SQL =
      "INSERT OR IGNORE INTO ai_cloud_usage_events("
      "event_id, provider, model_id, prompt_tokens, response_tokens, total_tokens, created_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare cloud usage restore failed");
  }
  const auto event_id = event.at("event_id").get<std::string>();
  const auto provider = event.at("provider").get<std::string>();
  const auto model_id = event.at("model_id").get<std::string>();
  sqlite3_bind_text(stmt, 1, event_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, model_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 4, event.at("prompt_tokens").get<long long>());
  sqlite3_bind_int64(stmt, 5, event.at("response_tokens").get<long long>());
  sqlite3_bind_int64(stmt, 6, event.at("total_tokens").get<long long>());
  sqlite3_bind_int64(stmt, 7, event.at("created_at").get<long long>());
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("restore cloud usage event failed");
}

void append_durable_event(const nlohmann::json& event) {
  std::lock_guard lock(usage_ledger_mutex);
  if (!usage_ledger_path.has_value()) return;
  auto body = load_ledger(*usage_ledger_path);
  const auto event_id = event.at("event_id").get<std::string>();
  for (const auto& existing : body.at("events")) {
    if (existing.value("event_id", std::string()) == event_id) return;
  }
  body["events"].push_back(event);
  write_ledger(*usage_ledger_path, body);
}

long long failure_cooldown_seconds(
    long long failure_count,
    long long base_seconds,
    long long cap_seconds
) {
  // Exponential backoff with a conservative cap.
  if (failure_count <= 0) return 0;
  const long long safe_base = std::max(1LL, base_seconds);
  const long long safe_cap = std::max(safe_base, cap_seconds);
  const long long bounded = std::min(failure_count - 1, 5LL);
  const long long raw = safe_base * (1LL << bounded);
  return std::min(raw, safe_cap);
}

} // namespace

void restore_cloud_usage_ledger(
    holder::platform::Db& db,
    const std::filesystem::path& path
) {
  std::lock_guard lock(usage_ledger_mutex);
  const auto body = load_ledger(path);
  for (const auto& event : body.at("events")) insert_event(db, event);
}

void initialize_cloud_usage_ledger(
    holder::platform::Db& db,
    const std::filesystem::path& path
) {
  std::lock_guard lock(usage_ledger_mutex);
  usage_ledger_path = path;
  if (std::filesystem::exists(path)) {
    const auto body = load_ledger(path);
    for (const auto& event : body.at("events")) insert_event(db, event);
    return;
  }

  nlohmann::json body = {{"version", 1}, {"events", nlohmann::json::array()}};
  sqlite3_stmt* stmt = nullptr;
  static constexpr const char* SQL =
      "SELECT event_id, provider, model_id, prompt_tokens, response_tokens, total_tokens, created_at "
      "FROM ai_cloud_usage_events ORDER BY created_at, event_id;";
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare cloud usage export failed");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    body["events"].push_back({
        {"event_id", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0))},
        {"provider", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))},
        {"model_id", reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))},
        {"prompt_tokens", sqlite3_column_int64(stmt, 3)},
        {"response_tokens", sqlite3_column_int64(stmt, 4)},
        {"total_tokens", sqlite3_column_int64(stmt, 5)},
        {"created_at", sqlite3_column_int64(stmt, 6)},
    });
  }
  sqlite3_finalize(stmt);
  write_ledger(path, body);
}

CloudQuotaWindowUsage load_cloud_window_usage(
    holder::platform::Db& db,
    const std::string& provider,
    const std::string& model_id,
    long long since_epoch_seconds
) {
  static constexpr const char* SQL = "SELECT COUNT(*), COALESCE(SUM(total_tokens), 0) "
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

void record_cloud_usage_event(
    holder::platform::Db& db,
    const std::string& provider,
    const std::string& model_id,
    long long prompt_tokens,
    long long response_tokens,
    long long created_at,
    const std::string& event_id_seed
) {
  static constexpr const char* SQL =
      "INSERT INTO ai_cloud_usage_events("
      "event_id, provider, model_id, prompt_tokens, response_tokens, total_tokens, created_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?);";

  const std::string event_id = "cloud-" + event_id_seed + "-" + std::to_string(created_at) + "-" +
                               std::to_string(static_cast<unsigned long long>(std::rand()));
  const long long total_tokens = prompt_tokens + response_tokens;
  append_durable_event({
      {"event_id", event_id},
      {"provider", provider},
      {"model_id", model_id},
      {"prompt_tokens", prompt_tokens},
      {"response_tokens", response_tokens},
      {"total_tokens", total_tokens},
      {"created_at", created_at},
  });
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

std::optional<CloudModelCooldownState> load_cloud_model_cooldown(
    holder::platform::Db& db,
    const std::string& provider,
    const std::string& model_id
) {
  static constexpr const char* SQL =
      "SELECT provider, model_id, failure_count, cooldown_until, COALESCE(last_error, ''), updated_at "
      "FROM ai_cloud_model_cooldowns "
      "WHERE provider = ? AND model_id = ?;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare cloud cooldown query failed");
  }
  sqlite3_bind_text(stmt, 1, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, model_id.c_str(), -1, SQLITE_TRANSIENT);

  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  CloudModelCooldownState out;
  out.provider = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  out.model_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  out.failure_count = sqlite3_column_int64(stmt, 2);
  out.cooldown_until = sqlite3_column_int64(stmt, 3);
  const auto* last_error = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  out.last_error = last_error ? std::string(last_error) : std::string();
  out.updated_at = sqlite3_column_int64(stmt, 5);

  sqlite3_finalize(stmt);
  return out;
}

CloudModelCooldownState record_cloud_model_failure(
    holder::platform::Db& db,
    const std::string& provider,
    const std::string& model_id,
    const std::string& error,
    long long now_epoch_seconds,
    long long cooldown_base_seconds,
    long long cooldown_cap_seconds
) {
  long long failure_count = 1;
  if (const auto current = load_cloud_model_cooldown(db, provider, model_id); current.has_value()) {
    failure_count = std::max(1LL, current->failure_count + 1);
  }
  const long long cooldown_seconds =
      failure_cooldown_seconds(failure_count, cooldown_base_seconds, cooldown_cap_seconds);
  const long long cooldown_until = now_epoch_seconds + cooldown_seconds;

  static constexpr const char* SQL =
      "INSERT INTO ai_cloud_model_cooldowns("
      "provider, model_id, failure_count, cooldown_until, last_error, updated_at) "
      "VALUES(?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(provider, model_id) DO UPDATE SET "
      "failure_count = excluded.failure_count, "
      "cooldown_until = excluded.cooldown_until, "
      "last_error = excluded.last_error, "
      "updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare cloud cooldown upsert failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, model_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, failure_count);
  sqlite3_bind_int64(stmt, 4, cooldown_until);
  sqlite3_bind_text(stmt, 5, error.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, now_epoch_seconds);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error("upsert cloud cooldown failed");
  }

  CloudModelCooldownState out;
  out.provider = provider;
  out.model_id = model_id;
  out.failure_count = failure_count;
  out.cooldown_until = cooldown_until;
  out.last_error = error;
  out.updated_at = now_epoch_seconds;
  return out;
} // LCOV_EXCL_LINE

void clear_cloud_model_cooldown(
    holder::platform::Db& db,
    const std::string& provider,
    const std::string& model_id,
    long long now_epoch_seconds
) {
  static constexpr const char* SQL =
      "INSERT INTO ai_cloud_model_cooldowns("
      "provider, model_id, failure_count, cooldown_until, last_error, updated_at) "
      "VALUES(?, ?, 0, 0, '', ?) "
      "ON CONFLICT(provider, model_id) DO UPDATE SET "
      "failure_count = 0, cooldown_until = 0, last_error = '', updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare cloud cooldown clear failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, provider.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, model_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 3, now_epoch_seconds);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw std::runtime_error("clear cloud cooldown failed");
  }
}

} // namespace holder::api::support
