#include "ai/AiNudgeDurability.h"

#include "git/GitOps.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectManifest.h"
#include "project/ProjectRepo.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace holder::ai {
namespace {

struct DismissedNudge {
  std::string nudge_id;
  std::string kind;
  std::string project_id;
  std::optional<std::string> card_id;
  std::string title;
  std::string body;
  std::string meta_json;
  std::optional<std::string> basis_fingerprint;
  std::optional<std::string> basis_commit;
  long long created_at = 0;
  long long dismissed_at = 0;
};

std::filesystem::path relative_path(const std::string& nudge_id) {
  if (nudge_id.size() < 4) throw std::invalid_argument("nudge_id too short for tombstone path");
  return std::filesystem::path("ai_nudge_dismissals") / nudge_id.substr(0, 2) /
         nudge_id.substr(2, 2) / (nudge_id + ".json");
}

std::optional<std::string> nullable_text(sqlite3_stmt* stmt, int column) {
  if (sqlite3_column_type(stmt, column) == SQLITE_NULL) return std::nullopt;
  return reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
}

DismissedNudge row(sqlite3_stmt* stmt) {
  DismissedNudge out;
  out.nudge_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  out.kind = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  out.project_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
  out.card_id = nullable_text(stmt, 3);
  out.title = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
  out.body = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
  out.meta_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
  out.basis_fingerprint = nullable_text(stmt, 7);
  out.basis_commit = nullable_text(stmt, 8);
  out.created_at = sqlite3_column_int64(stmt, 9);
  out.dismissed_at = sqlite3_column_int64(stmt, 10);
  return out;
}

nlohmann::json as_json(const DismissedNudge& nudge) {
  return {
      {"version", 1},
      {"nudge_id", nudge.nudge_id},
      {"kind", nudge.kind},
      {"project_id", nudge.project_id},
      {"card_id", nudge.card_id ? nlohmann::json(*nudge.card_id) : nlohmann::json(nullptr)},
      {"title", nudge.title},
      {"body", nudge.body},
      {"meta_json", nlohmann::json::parse(nudge.meta_json)},
      {"basis_fingerprint", nudge.basis_fingerprint
                                ? nlohmann::json(*nudge.basis_fingerprint)
                                : nlohmann::json(nullptr)},
      {"basis_commit", nudge.basis_commit ? nlohmann::json(*nudge.basis_commit)
                                            : nlohmann::json(nullptr)},
      {"created_at", nudge.created_at},
      {"dismissed_at", nudge.dismissed_at},
  };
}

DismissedNudge from_json(const nlohmann::json& body) {
  if (body.value("version", 0) != 1) {
    throw std::runtime_error("unsupported AI nudge dismissal version");
  }
  DismissedNudge out;
  out.nudge_id = body.at("nudge_id").get<std::string>();
  out.kind = body.at("kind").get<std::string>();
  out.project_id = body.at("project_id").get<std::string>();
  if (!body.at("card_id").is_null()) out.card_id = body.at("card_id").get<std::string>();
  out.title = body.at("title").get<std::string>();
  out.body = body.at("body").get<std::string>();
  out.meta_json = body.at("meta_json").dump();
  if (!body.at("basis_fingerprint").is_null())
    out.basis_fingerprint = body.at("basis_fingerprint").get<std::string>();
  if (!body.at("basis_commit").is_null())
    out.basis_commit = body.at("basis_commit").get<std::string>();
  out.created_at = body.at("created_at").get<long long>();
  out.dismissed_at = body.at("dismissed_at").get<long long>();
  if (out.nudge_id.empty() || out.kind.empty() || out.project_id.empty() || out.dismissed_at <= 0) {
    throw std::runtime_error("invalid AI nudge dismissal");
  }
  return out;
}

std::string encode(const holder::model::Project& project, const DismissedNudge& nudge) {
  const auto plain = as_json(nudge).dump(2) + '\n';
  if (project.privacy_mode != "encrypted_git") return plain;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project has no key for AI nudge dismissal");
  }
  return holder::privacy::encrypt_project_blob(project.project_id, *project.project_key_id, plain);
}

DismissedNudge decode(const holder::model::Project& project, const std::string& raw) {
  auto plain = raw;
  if (project.privacy_mode == "encrypted_git") {
    if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
      throw std::runtime_error("encrypted project has no key for AI nudge dismissal");
    }
    plain = holder::privacy::decrypt_project_blob(project.project_id, *project.project_key_id, raw);
  }
  return from_json(nlohmann::json::parse(plain));
}

std::optional<DismissedNudge> find(holder::platform::Db& db, const std::string& nudge_id) {
  static constexpr const char* SQL =
      "SELECT nudge_id, kind, project_id, card_id, title, body, meta_json, basis_fingerprint, "
      "basis_commit, created_at, dismissed_at FROM ai_nudges "
      "WHERE nudge_id=? AND dismissed_at IS NOT NULL;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare nudge dismissal lookup failed");
  }
  sqlite3_bind_text(stmt, 1, nudge_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    auto out = row(stmt);
    sqlite3_finalize(stmt);
    return out;
  }
  sqlite3_finalize(stmt);
  return std::nullopt;
}

void insert(holder::platform::Db& db, const DismissedNudge& nudge) {
  static constexpr const char* SQL =
      "INSERT OR REPLACE INTO ai_nudges(nudge_id, kind, project_id, card_id, title, body, "
      "meta_json, basis_fingerprint, basis_commit, created_at, dismissed_at) "
      "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare nudge dismissal restore failed");
  }
  sqlite3_bind_text(stmt, 1, nudge.nudge_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, nudge.kind.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, nudge.project_id.c_str(), -1, SQLITE_TRANSIENT);
  if (nudge.card_id) sqlite3_bind_text(stmt, 4, nudge.card_id->c_str(), -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt, 4);
  sqlite3_bind_text(stmt, 5, nudge.title.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 6, nudge.body.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 7, nudge.meta_json.c_str(), -1, SQLITE_TRANSIENT);
  if (nudge.basis_fingerprint)
    sqlite3_bind_text(stmt, 8, nudge.basis_fingerprint->c_str(), -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt, 8);
  if (nudge.basis_commit)
    sqlite3_bind_text(stmt, 9, nudge.basis_commit->c_str(), -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt, 9);
  sqlite3_bind_int64(stmt, 10, nudge.created_at);
  sqlite3_bind_int64(stmt, 11, nudge.dismissed_at);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("restore nudge dismissal failed");
}

} // namespace

bool persist_nudge_dismissal(holder::platform::Db& db, const std::string& nudge_id) {
  const auto nudge = find(db, nudge_id);
  if (!nudge.has_value()) return false;
  const auto project = holder::project::ProjectRepo(db).get(nudge->project_id);
  if (!project.has_value()) throw std::runtime_error("project missing for nudge dismissal");
  if (!holder::project::has_project_manifest(project->root_path)) return false;
  holder::git::RealGitOps git;
  const auto path = relative_path(nudge_id);
  git.open_or_init(project->root_path);
  git.write_file(path, encode(*project, *nudge));
  git.stage_path(path);
  git.commit("Record AI nudge dismissal");
  return true;
}

std::size_t backfill_nudge_dismissals(holder::platform::Db& db) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(
          db.handle(),
          "SELECT nudge_id FROM ai_nudges WHERE dismissed_at IS NOT NULL ORDER BY nudge_id;",
          -1, &stmt, nullptr
      ) != SQLITE_OK) {
    throw std::runtime_error("prepare nudge dismissal backfill failed");
  }
  std::size_t count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const auto nudge = find(db, id);
    if (!nudge.has_value()) continue;
    const auto project = holder::project::ProjectRepo(db).get(nudge->project_id);
    if (!project.has_value()) continue;
    if (std::filesystem::is_regular_file(
            std::filesystem::path(project->root_path) / relative_path(id)
        )) continue;
    if (persist_nudge_dismissal(db, id)) ++count;
  }
  sqlite3_finalize(stmt);
  return count;
}

std::size_t restore_nudge_dismissals(holder::platform::Db& db) {
  std::size_t count = 0;
  for (const auto& project : holder::project::ProjectRepo(db).list()) {
    const auto root = std::filesystem::path(project.root_path) / "ai_nudge_dismissals";
    if (!std::filesystem::exists(root)) continue;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
      std::ifstream in(entry.path(), std::ios::binary);
      if (!in) throw std::runtime_error("failed to open nudge dismissal: " + entry.path().string());
      std::ostringstream raw;
      raw << in.rdbuf();
      const auto nudge = decode(project, raw.str());
      if (nudge.project_id != project.project_id ||
          entry.path().filename() != nudge.nudge_id + ".json") {
        throw std::runtime_error("nudge dismissal path or project mismatch");
      }
      insert(db, nudge);
      ++count;
    }
  }
  return count;
}

bool all_nudge_dismissals_are_durable(holder::platform::Db& db) {
  sqlite3_stmt* stmt = nullptr;
  static constexpr const char* SQL =
      "SELECT n.nudge_id, p.root_path FROM ai_nudges n "
      "JOIN projects p ON p.project_id=n.project_id "
      "WHERE n.dismissed_at IS NOT NULL ORDER BY n.nudge_id;";
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare nudge dismissal ownership audit failed");
  }
  bool durable = true;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const std::string id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    const std::filesystem::path root =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!std::filesystem::is_regular_file(root / relative_path(id))) {
      durable = false;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return durable;
}

} // namespace holder::ai
