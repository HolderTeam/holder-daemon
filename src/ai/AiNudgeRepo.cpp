#include "ai/AiNudgeRepo.h"

#include "ai/AiNudgeDurability.h"

#include <sqlite3.h>

#include <stdexcept>

namespace holder::ai {
namespace {

std::string column_text(sqlite3_stmt* stmt, int index) {
  const unsigned char* text = sqlite3_column_text(stmt, index);
  if (!text) return {};
  return reinterpret_cast<const char*>(text);
} // LCOV_EXCL_LINE

std::optional<std::string> column_nullable(sqlite3_stmt* stmt, int index) {
  if (sqlite3_column_type(stmt, index) == SQLITE_NULL) return std::nullopt;
  return column_text(stmt, index);
} // LCOV_EXCL_LINE

void bind_nullable_text(sqlite3_stmt* stmt, int index, const std::optional<std::string>& value) {
  if (value.has_value()) {
    sqlite3_bind_text(stmt, index, value->c_str(), -1, SQLITE_TRANSIENT);
  } else {
    sqlite3_bind_null(stmt, index);
  }
}

void throw_sqlite(sqlite3* db, const std::string& msg) { // LCOV_EXCL_LINE
  throw std::runtime_error(msg + ": " + sqlite3_errmsg(db)); // LCOV_EXCL_LINE
} // LCOV_EXCL_LINE

nlohmann::json column_json(sqlite3_stmt* stmt, int index) {
  const auto text = column_text(stmt, index);
  return text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text);
}

Nudge row_to_nudge(sqlite3_stmt* stmt) {
  return {
      .nudge_id = column_text(stmt, 0),
      .kind = column_text(stmt, 1),
      .project_id = column_text(stmt, 2),
      .card_id = column_nullable(stmt, 3),
      .title = column_text(stmt, 4),
      .body = column_text(stmt, 5),
      .meta_json = column_json(stmt, 6),
      .basis_fingerprint = column_nullable(stmt, 7),
      .basis_commit = column_nullable(stmt, 8),
      .created_at = sqlite3_column_int64(stmt, 9),
      .dismissed = sqlite3_column_type(stmt, 10) != SQLITE_NULL,
  };
}

} // namespace

AiNudgeRepo::AiNudgeRepo(holder::platform::Db& db)
    : db_(db) {}

void AiNudgeRepo::create(const Nudge& nudge) {
  static constexpr const char* SQL =
      "INSERT INTO ai_nudges("
      "nudge_id, kind, project_id, card_id, title, body, meta_json, basis_fingerprint, basis_commit, created_at, dismissed_at"
      ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_nudges insert failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, nudge.nudge_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, nudge.kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, nudge.project_id.c_str(), -1, SQLITE_TRANSIENT);
  bind_nullable_text(stmt, 4, nudge.card_id);
  sqlite3_bind_text(stmt, 5, nudge.title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, nudge.body.c_str(), -1, SQLITE_TRANSIENT);
  const auto meta_json = nudge.meta_json.is_null() ? std::string("{}") : nudge.meta_json.dump();
  sqlite3_bind_text(stmt, 7, meta_json.c_str(), -1, SQLITE_TRANSIENT);
  bind_nullable_text(stmt, 8, nudge.basis_fingerprint);
  bind_nullable_text(stmt, 9, nudge.basis_commit);
  sqlite3_bind_int64(stmt, 10, nudge.created_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "insert ai_nudge failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
}

Nudge AiNudgeRepo::create_or_get(const Nudge& nudge) {
  static constexpr const char* SQL =
      "INSERT OR IGNORE INTO ai_nudges("
      "nudge_id, kind, project_id, card_id, title, body, meta_json, basis_fingerprint, basis_commit, created_at, dismissed_at"
      ") VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, NULL);";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_nudges insert failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, nudge.nudge_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, nudge.kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, nudge.project_id.c_str(), -1, SQLITE_TRANSIENT);
  bind_nullable_text(stmt, 4, nudge.card_id);
  sqlite3_bind_text(stmt, 5, nudge.title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, nudge.body.c_str(), -1, SQLITE_TRANSIENT);
  const auto meta_json = nudge.meta_json.is_null() ? std::string("{}") : nudge.meta_json.dump();
  sqlite3_bind_text(stmt, 7, meta_json.c_str(), -1, SQLITE_TRANSIENT);
  bind_nullable_text(stmt, 8, nudge.basis_fingerprint);
  bind_nullable_text(stmt, 9, nudge.basis_commit);
  sqlite3_bind_int64(stmt, 10, nudge.created_at);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "insert ai_nudge failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);

  auto stored = find_by_id(nudge.nudge_id);
  if (!stored.has_value()) {
    throw std::runtime_error("insert ai_nudge failed: row not found after insert"
    ); // LCOV_EXCL_LINE
  }
  return stored.value();
}

std::optional<Nudge> AiNudgeRepo::find_by_id(const std::string& nudge_id) const {
  static constexpr const char* SQL =
      "SELECT nudge_id, kind, project_id, card_id, title, body, meta_json, basis_fingerprint, basis_commit, created_at, dismissed_at "
      "FROM ai_nudges WHERE nudge_id = ? LIMIT 1;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_nudges find by id failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, nudge_id.c_str(), -1, SQLITE_TRANSIENT);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto nudge = row_to_nudge(stmt);
    sqlite3_finalize(stmt);
    return nudge;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) {
    return std::nullopt;
  }
  throw_sqlite(db_.handle(), "query ai_nudges by id failed"); // LCOV_EXCL_LINE
  return std::nullopt; // LCOV_EXCL_LINE
}

std::optional<Nudge> AiNudgeRepo::find_active_exact_match(
    const std::string& kind,
    const std::string& project_id,
    const std::optional<std::string>& card_id,
    const std::optional<std::string>& basis_fingerprint,
    const std::optional<std::string>& basis_commit
) const {
  static constexpr const char* SQL =
      "SELECT nudge_id, kind, project_id, card_id, title, body, meta_json, basis_fingerprint, basis_commit, created_at, dismissed_at "
      "FROM ai_nudges WHERE kind = ? AND project_id = ? "
      "AND ((card_id IS NULL AND ? IS NULL) OR card_id = ?) "
      "AND ((basis_fingerprint IS NULL AND ? IS NULL) OR basis_fingerprint = ?) "
      "AND ((basis_commit IS NULL AND ? IS NULL) OR basis_commit = ?) "
      "AND dismissed_at IS NULL LIMIT 1;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_nudges exact match failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, project_id.c_str(), -1, SQLITE_TRANSIENT);
  bind_nullable_text(stmt, 3, card_id);
  bind_nullable_text(stmt, 4, card_id);
  bind_nullable_text(stmt, 5, basis_fingerprint);
  bind_nullable_text(stmt, 6, basis_fingerprint);
  bind_nullable_text(stmt, 7, basis_commit);
  bind_nullable_text(stmt, 8, basis_commit);

  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto nudge = row_to_nudge(stmt);
    sqlite3_finalize(stmt);
    return nudge;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE) {
    return std::nullopt;
  }
  throw_sqlite(db_.handle(), "query ai_nudges exact match failed"); // LCOV_EXCL_LINE
  return std::nullopt; // LCOV_EXCL_LINE
}

std::vector<Nudge> AiNudgeRepo::list_active(
    const std::string& project_id,
    const std::optional<std::string>& card_id
) const {
  static constexpr const char* SQL_FOR_PROJECT =
      "SELECT nudge_id, kind, project_id, card_id, title, body, meta_json, basis_fingerprint, basis_commit, created_at, dismissed_at "
      "FROM ai_nudges WHERE project_id = ? AND card_id IS NULL AND dismissed_at IS NULL "
      "ORDER BY created_at DESC;";
  static constexpr const char* SQL_FOR_CARD =
      "SELECT nudge_id, kind, project_id, card_id, title, body, meta_json, basis_fingerprint, basis_commit, created_at, dismissed_at "
      "FROM ai_nudges WHERE project_id = ? AND dismissed_at IS NULL AND (card_id IS NULL OR card_id = ?) "
      "ORDER BY created_at DESC;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_.handle(),
          card_id.has_value() ? SQL_FOR_CARD : SQL_FOR_PROJECT,
          -1,
          &stmt,
          nullptr
      ) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_nudges list failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, project_id.c_str(), -1, SQLITE_TRANSIENT);
  if (card_id.has_value()) {
    sqlite3_bind_text(stmt, 2, card_id->c_str(), -1, SQLITE_TRANSIENT);
  }

  std::vector<Nudge> out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    out.push_back(row_to_nudge(stmt));
  }
  sqlite3_finalize(stmt);
  return out;
} // LCOV_EXCL_LINE

void AiNudgeRepo::dismiss_stale_variants(
    const std::string& kind,
    const std::string& project_id,
    const std::optional<std::string>& card_id,
    const std::optional<std::string>& basis_fingerprint,
    const std::optional<std::string>& basis_commit
) {
  static constexpr const char* SQL =
      "UPDATE ai_nudges "
      "SET dismissed_at = CAST(strftime('%s','now') AS INTEGER) "
      "WHERE kind = ? AND project_id = ? "
      "AND ((card_id IS NULL AND ? IS NULL) OR card_id = ?) "
      "AND dismissed_at IS NULL "
      "AND NOT (((basis_fingerprint IS NULL AND ? IS NULL) OR basis_fingerprint = ?) "
      "AND ((basis_commit IS NULL AND ? IS NULL) OR basis_commit = ?));";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_nudges dismiss stale failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, project_id.c_str(), -1, SQLITE_TRANSIENT);
  bind_nullable_text(stmt, 3, card_id);
  bind_nullable_text(stmt, 4, card_id);
  bind_nullable_text(stmt, 5, basis_fingerprint);
  bind_nullable_text(stmt, 6, basis_fingerprint);
  bind_nullable_text(stmt, 7, basis_commit);
  bind_nullable_text(stmt, 8, basis_commit);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "dismiss stale ai_nudges failed"); // LCOV_EXCL_LINE
  }
  sqlite3_finalize(stmt);
  backfill_nudge_dismissals(db_);
}

bool AiNudgeRepo::dismiss(const std::string& nudge_id) {
  static constexpr const char* SQL = "UPDATE ai_nudges "
                                     "SET dismissed_at = CAST(strftime('%s','now') AS INTEGER) "
                                     "WHERE nudge_id = ? AND dismissed_at IS NULL;";

  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw_sqlite(db_.handle(), "prepare ai_nudges dismiss failed"); // LCOV_EXCL_LINE
  }
  sqlite3_bind_text(stmt, 1, nudge_id.c_str(), -1, SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_DONE) {
    sqlite3_finalize(stmt); // LCOV_EXCL_LINE
    throw_sqlite(db_.handle(), "dismiss ai_nudge failed"); // LCOV_EXCL_LINE
  }
  const auto changed = sqlite3_changes(db_.handle());
  sqlite3_finalize(stmt);
  if (changed > 0) persist_nudge_dismissal(db_, nudge_id);
  return changed > 0;
}

} // namespace holder::ai
