#include "ai/AiLocalModelConfigRepo.h"

#include "llm/RunnerModelRef.h"

#include <sqlite3.h>

#include <stdexcept>

namespace holder::ai {
namespace {

void throw_sqlite(sqlite3* db, const std::string& what) {
  const char* msg = db ? sqlite3_errmsg(db) : "unknown sqlite error";
  throw std::runtime_error(what + ": " + msg);
}

void bind_optional_text(sqlite3_stmt* stmt, int idx, const std::optional<std::string>& value) {
  if (!value.has_value() || value->empty()) {
    if (sqlite3_bind_null(stmt, idx) != SQLITE_OK) {
      throw std::runtime_error("sqlite bind_null failed"); // LCOV_EXCL_LINE
    }
    return;
  }
  if (sqlite3_bind_text(stmt, idx, value->c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    throw std::runtime_error("sqlite bind_text failed"); // LCOV_EXCL_LINE
  }
}

std::optional<std::string> column_optional_text(sqlite3_stmt* stmt, int idx) {
  if (sqlite3_column_type(stmt, idx) == SQLITE_NULL) {
    return std::nullopt;
  }
  return std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, idx)));
}

holder::model::AiLocalModelConfig read_config(sqlite3_stmt* stmt) {
  holder::model::AiLocalModelConfig out;
  const auto fast_model = column_optional_text(stmt, 0);
  const auto strong_model = column_optional_text(stmt, 1);
  const auto deep_model = column_optional_text(stmt, 2);
  out.fast_model = fast_model.has_value()
                       ? std::optional<std::string>(
                             holder::llm::normalize_local_runner_model_ref(fast_model.value()))
                       : std::nullopt;
  out.strong_model = strong_model.has_value()
                         ? std::optional<std::string>(
                               holder::llm::normalize_local_runner_model_ref(strong_model.value()))
                         : std::nullopt;
  out.deep_model = deep_model.has_value()
                       ? std::optional<std::string>(
                             holder::llm::normalize_local_runner_model_ref(deep_model.value()))
                       : std::nullopt;
  out.updated_at = sqlite3_column_int64(stmt, 3);
  return out;
}

bool has_any_model(const std::optional<std::string>& fast_model,
                   const std::optional<std::string>& strong_model,
                   const std::optional<std::string>& deep_model) {
  return (fast_model.has_value() && !fast_model->empty()) ||
         (strong_model.has_value() && !strong_model->empty()) ||
         (deep_model.has_value() && !deep_model->empty());
}

} // namespace

AiLocalModelConfigRepo::AiLocalModelConfigRepo(holder::platform::Db& db) : db_(db) {}

std::optional<holder::model::AiLocalModelConfig> AiLocalModelConfigRepo::get() const {
  static constexpr const char* SQL =
      "SELECT fast_model, strong_model, deep_model, updated_at "
      "FROM ai_local_model_config WHERE key = 'global';";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare get local model config failed");
  }

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto cfg = read_config(stmt);
    sqlite3_finalize(stmt);
    return cfg;
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    throw_sqlite(db_.handle(), "get local model config failed");
  }
  return std::nullopt;
}

void AiLocalModelConfigRepo::set(const std::optional<std::string>& fast_model,
                                 const std::optional<std::string>& strong_model,
                                 const std::optional<std::string>& deep_model,
                                 long long updated_at) {
  if (!has_any_model(fast_model, strong_model, deep_model)) {
    clear();
    return;
  }

  const auto normalized_fast_model =
      fast_model.has_value() ? std::optional<std::string>(
                                   holder::llm::normalize_local_runner_model_ref(fast_model.value()))
                             : std::nullopt;
  const auto normalized_strong_model =
      strong_model.has_value() ? std::optional<std::string>(
                                     holder::llm::normalize_local_runner_model_ref(strong_model.value()))
                               : std::nullopt;
  const auto normalized_deep_model =
      deep_model.has_value() ? std::optional<std::string>(
                                   holder::llm::normalize_local_runner_model_ref(deep_model.value()))
                             : std::nullopt;

  static constexpr const char* SQL =
      "INSERT INTO ai_local_model_config(key, fast_model, strong_model, deep_model, updated_at) "
      "VALUES('global', ?, ?, ?, ?) "
      "ON CONFLICT(key) DO UPDATE SET "
      "fast_model = excluded.fast_model, "
      "strong_model = excluded.strong_model, "
      "deep_model = excluded.deep_model, "
      "updated_at = excluded.updated_at;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare upsert local model config failed"); // LCOV_EXCL_LINE
  }

  bind_optional_text(stmt, 1, normalized_fast_model);
  bind_optional_text(stmt, 2, normalized_strong_model);
  bind_optional_text(stmt, 3, normalized_deep_model);
  sqlite3_bind_int64(stmt, 4, updated_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "upsert local model config failed");
  }
  sqlite3_finalize(stmt);
}

void AiLocalModelConfigRepo::clear() {
  static constexpr const char* SQL = "DELETE FROM ai_local_model_config WHERE key = 'global';";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare clear local model config failed"); // LCOV_EXCL_LINE
  }
  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt);
    throw_sqlite(db_.handle(), "clear local model config failed");
  }
  sqlite3_finalize(stmt);
}

} // namespace holder::ai
