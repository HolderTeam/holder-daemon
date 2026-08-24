#include "ai/AiThreadStateDurability.h"

#include "ai/AiThreadRepo.h"
#include "git/GitOps.h"
#include "privacy/ProjectPrivacy.h"
#include "project/ProjectManifest.h"
#include "project/ProjectRepo.h"

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace holder::ai {
namespace {

std::filesystem::path relative_path(const std::string& thread_id) {
  if (thread_id.size() < 4) throw std::invalid_argument("thread_id too short for state path");
  return std::filesystem::path("ai_thread_state") / thread_id.substr(0, 2) /
         thread_id.substr(2, 2) / (thread_id + ".json");
}

std::string encode(
    const holder::model::Project& project,
    const holder::api::support::ThreadCompactionState& state
) {
  nlohmann::json body = {
      {"version", 1},
      {"thread_id", state.thread_id},
      {"rolling_summary", state.rolling_summary.has_value()
                              ? nlohmann::json(*state.rolling_summary)
                              : nlohmann::json(nullptr)},
      {"pinned_facts_json", state.pinned_facts_json.has_value()
                                ? nlohmann::json(*state.pinned_facts_json)
                                : nlohmann::json(nullptr)},
      {"last_compacted_message_id", state.last_compacted_message_id.has_value()
                                          ? nlohmann::json(*state.last_compacted_message_id)
                                          : nlohmann::json(nullptr)},
      {"updated_at", state.updated_at},
  };
  const auto plain = body.dump(2) + '\n';
  if (project.privacy_mode != "encrypted_git") return plain;
  if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
    throw std::runtime_error("encrypted project has no key for AI thread state");
  }
  return holder::privacy::encrypt_project_blob(project.project_id, *project.project_key_id, plain);
}

holder::api::support::ThreadCompactionState decode(
    const holder::model::Project& project,
    const std::string& raw
) {
  auto plain = raw;
  if (project.privacy_mode == "encrypted_git") {
    if (!project.project_key_id.has_value() || project.project_key_id->empty()) {
      throw std::runtime_error("encrypted project has no key for AI thread state");
    }
    plain = holder::privacy::decrypt_project_blob(project.project_id, *project.project_key_id, raw);
  }
  const auto body = nlohmann::json::parse(plain);
  if (body.value("version", 0) != 1) {
    throw std::runtime_error("unsupported AI thread state version");
  }
  holder::api::support::ThreadCompactionState state;
  state.thread_id = body.at("thread_id").get<std::string>();
  if (body.contains("rolling_summary") && !body.at("rolling_summary").is_null()) {
    state.rolling_summary = body.at("rolling_summary").get<std::string>();
  }
  if (body.contains("pinned_facts_json") && !body.at("pinned_facts_json").is_null()) {
    state.pinned_facts_json = body.at("pinned_facts_json").get<std::string>();
  }
  if (body.contains("last_compacted_message_id") &&
      !body.at("last_compacted_message_id").is_null()) {
    state.last_compacted_message_id = body.at("last_compacted_message_id").get<std::string>();
  }
  state.updated_at = body.at("updated_at").get<long long>();
  if (state.thread_id.empty()) throw std::runtime_error("AI thread state has no thread_id");
  return state;
}

void upsert_projection(
    holder::platform::Db& db,
    const holder::api::support::ThreadCompactionState& state
) {
  static constexpr const char* SQL =
      "INSERT INTO ai_thread_compaction_state("
      "thread_id, rolling_summary, pinned_facts_json, last_compacted_message_id, updated_at) "
      "VALUES(?, ?, ?, ?, ?) ON CONFLICT(thread_id) DO UPDATE SET "
      "rolling_summary=excluded.rolling_summary, pinned_facts_json=excluded.pinned_facts_json, "
      "last_compacted_message_id=excluded.last_compacted_message_id, updated_at=excluded.updated_at;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare AI thread state restore failed");
  }
  sqlite3_bind_text(stmt, 1, state.thread_id.c_str(), -1, SQLITE_TRANSIENT);
  if (state.rolling_summary) sqlite3_bind_text(stmt, 2, state.rolling_summary->c_str(), -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt, 2);
  if (state.pinned_facts_json) sqlite3_bind_text(stmt, 3, state.pinned_facts_json->c_str(), -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt, 3);
  if (state.last_compacted_message_id) sqlite3_bind_text(stmt, 4, state.last_compacted_message_id->c_str(), -1, SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt, 4);
  sqlite3_bind_int64(stmt, 5, state.updated_at);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) throw std::runtime_error("restore AI thread state failed");
}

} // namespace

bool persist_thread_compaction_state(
    holder::platform::Db& db,
    const holder::api::support::ThreadCompactionState& state
) {
  const auto thread = holder::ai::AiThreadRepo(db).get(state.thread_id);
  if (!thread.has_value()) return false;
  const auto project = holder::project::ProjectRepo(db).get(thread->project_id);
  if (!project.has_value()) throw std::runtime_error("project missing for AI thread state");
  if (!holder::project::has_project_manifest(project->root_path)) return false;
  holder::git::RealGitOps git;
  const auto path = relative_path(state.thread_id);
  git.open_or_init(project->root_path);
  git.write_file(path, encode(*project, state));
  git.stage_path(path);
  git.commit("Update durable AI thread state");
  return true;
}

std::size_t backfill_thread_compaction_states(holder::platform::Db& db) {
  sqlite3_stmt* stmt = nullptr;
  static constexpr const char* SQL =
      "SELECT thread_id, rolling_summary, pinned_facts_json, last_compacted_message_id, updated_at "
      "FROM ai_thread_compaction_state ORDER BY thread_id;";
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare AI thread state backfill failed");
  }
  std::size_t count = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    holder::api::support::ThreadCompactionState state;
    state.thread_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (sqlite3_column_type(stmt, 1) != SQLITE_NULL)
      state.rolling_summary = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (sqlite3_column_type(stmt, 2) != SQLITE_NULL)
      state.pinned_facts_json = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL)
      state.last_compacted_message_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    state.updated_at = sqlite3_column_int64(stmt, 4);
    const auto thread = holder::ai::AiThreadRepo(db).get(state.thread_id);
    if (!thread.has_value()) continue;
    const auto project = holder::project::ProjectRepo(db).get(thread->project_id);
    if (!project.has_value()) continue;
    if (std::filesystem::is_regular_file(
            std::filesystem::path(project->root_path) / relative_path(state.thread_id)
        )) continue;
    if (persist_thread_compaction_state(db, state)) ++count;
  }
  sqlite3_finalize(stmt);
  return count;
}

std::size_t restore_thread_compaction_states(holder::platform::Db& db) {
  std::size_t count = 0;
  for (const auto& project : holder::project::ProjectRepo(db).list()) {
    const auto root = std::filesystem::path(project.root_path) / "ai_thread_state";
    if (!std::filesystem::exists(root)) continue;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
      std::ifstream in(entry.path(), std::ios::binary);
      if (!in) throw std::runtime_error("failed to open AI thread state: " + entry.path().string());
      std::ostringstream raw;
      raw << in.rdbuf();
      const auto state = decode(project, raw.str());
      if (entry.path().filename() != state.thread_id + ".json") {
        throw std::runtime_error("AI thread state path does not match thread_id");
      }
      const auto thread = holder::ai::AiThreadRepo(db).get(state.thread_id);
      if (!thread.has_value() || thread->project_id != project.project_id) {
        throw std::runtime_error("AI thread state references an unknown thread");
      }
      upsert_projection(db, state);
      ++count;
    }
  }
  return count;
}

bool all_thread_compaction_states_are_durable(holder::platform::Db& db) {
  sqlite3_stmt* stmt = nullptr;
  static constexpr const char* SQL =
      "SELECT s.thread_id, p.root_path FROM ai_thread_compaction_state s "
      "JOIN ai_threads t ON t.thread_id=s.thread_id "
      "JOIN projects p ON p.project_id=t.project_id ORDER BY s.thread_id;";
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare AI thread state ownership audit failed");
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
