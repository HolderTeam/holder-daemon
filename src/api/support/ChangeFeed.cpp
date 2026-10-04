#include "api/support/ChangeFeed.h"
#include "git/GitRepo.h"

#include <openssl/evp.h>
#include <sqlite3.h>

#include <array>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

namespace holder::api::support {
namespace {

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement prepare(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error(sqlite3_errmsg(db));
  }
  return Statement(stmt, sqlite3_finalize);
}

nlohmann::json value(sqlite3_stmt* stmt, int column) {
  switch (sqlite3_column_type(stmt, column)) {
  case SQLITE_NULL:
    return nullptr;
  case SQLITE_INTEGER:
    return sqlite3_column_int64(stmt, column);
  case SQLITE_FLOAT:
    return sqlite3_column_double(stmt, column);
  default:
    if (sqlite3_column_bytes(stmt, column) == 0) return std::string{};
    return std::string(
        reinterpret_cast<const char*>(sqlite3_column_blob(stmt, column)),
        static_cast<std::size_t>(sqlite3_column_bytes(stmt, column))
    );
  }
}

struct Query {
  const char* kind;
  const char* sql;
  const char* optional_table = nullptr;
};

// IDs are for invalidation only. Related tables contribute to their owning
// entity, so same-second edits and changes to tags, links or indexed content
// are detected without relying on updated_at timestamps or HTTP responses.
const Query kQueries[] = {
    {"project", "SELECT project_id, project_id, * FROM projects ORDER BY project_id"},
    {"card", "SELECT card_id, project_id, * FROM cards ORDER BY card_id"},
    {"card", "SELECT card_id, project_id, * FROM cards_fts ORDER BY card_id, rowid"},
    {"card", "SELECT card_id, project_id, * FROM card_tags ORDER BY card_id, tag"},
    {"card", "SELECT card_id, project_id, * FROM milestones ORDER BY card_id, milestone_id"},
    {"project",
     "SELECT project_id, project_id, * FROM card_links ORDER BY project_id, from_card_id, to_card_id, to_type, kind"
    },
    {"project",
     "SELECT project_id, project_id, * FROM project_sync_state ORDER BY project_id",
     "project_sync_state"},
    {"thread", "SELECT thread_id, project_id, * FROM ai_threads ORDER BY thread_id"},
    {"message",
     "SELECT m.message_id, t.project_id, m.* FROM ai_messages m JOIN ai_threads t USING(thread_id) ORDER BY m.message_id"
    },
    {"run", "SELECT run_id, COALESCE(project_id, ''), * FROM ai_runs ORDER BY run_id"},
    {"resource", "SELECT resource_id, project_id, * FROM resources ORDER BY resource_id"},
    {"resource",
     "SELECT m.resource_id, r.project_id, m.* FROM resource_metadata m JOIN resources r USING(resource_id) ORDER BY m.resource_id, property, value_index"
    },
    {"resource", "SELECT resource_id, project_id, * FROM assets ORDER BY resource_id, asset_id"},
    {"resource",
     "SELECT a.resource_id, a.project_id, p.* FROM asset_placements p JOIN assets a USING(asset_id) ORDER BY a.resource_id, p.placement_id"
    },
    {"location", "SELECT location_id, project_id, * FROM storage_locations ORDER BY location_id"},
    {"project", "SELECT project_id, project_id, * FROM ai_nudges ORDER BY project_id, nudge_id"},
};

} // namespace

ChangeFeed::ChangeFeed(holder::platform::Db& db, EventJournal& journal)
    : db_(db),
      journal_(journal) {
  version_ = data_version();
  previous_ = snapshot();
  revisions_ = read_revisions();
}

nlohmann::json ChangeFeed::read_revisions() {
  auto result = nlohmann::json::object();
  auto stmt =
      prepare(db_.handle(), "SELECT project_id, root_path FROM projects ORDER BY project_id");
  int rc;
  while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
    const auto project = value(stmt.get(), 0).get<std::string>();
    result[project] = nullptr;
    try {
      holder::git::GitRepo repo;
      repo.open_existing(value(stmt.get(), 1).get<std::string>());
      const auto revision = repo.head_oid();
      if (revision) result[project] = *revision;
    } catch (const std::exception&) {
      // Missing/unborn/unavailable Git is reported as a null revision, never
      // initialized or repaired by this read-only observer.
    }
  }
  if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_.handle()));
  return result;
}

long long ChangeFeed::data_version() const {
  auto stmt = prepare(db_.handle(), "PRAGMA data_version");
  if (sqlite3_step(stmt.get()) != SQLITE_ROW)
    throw std::runtime_error(sqlite3_errmsg(db_.handle()));
  return sqlite3_column_int64(stmt.get(), 0);
}

ChangeFeed::Snapshot ChangeFeed::snapshot() {
  Snapshot result;
  using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
  std::map<Key, Digest> digests;
  db_.exec("BEGIN");
  try {
    for (const auto& query : kQueries) {
      if (query.optional_table) {
        auto table =
            prepare(db_.handle(), "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
        sqlite3_bind_text(table.get(), 1, query.optional_table, -1, SQLITE_STATIC);
        const auto rc = sqlite3_step(table.get());
        if (rc == SQLITE_DONE) continue;
        if (rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db_.handle()));
      }
      auto stmt = prepare(db_.handle(), query.sql);
      int rc;
      while ((rc = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        const Key key{
            query.kind,
            value(stmt.get(), 0).get<std::string>(),
            value(stmt.get(), 1).get<std::string>()
        };
        auto it = digests.find(key);
        if (it == digests.end()) {
          Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
          if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
            throw std::runtime_error("Cannot initialize change-feed fingerprint");
          it = digests.emplace(key, std::move(digest)).first;
        }
        auto row = nlohmann::json::array({query.sql});
        for (int column = 2; column < sqlite3_column_count(stmt.get()); ++column)
          row.push_back(value(stmt.get(), column));
        const auto encoded = row.dump();
        if (EVP_DigestUpdate(it->second.get(), encoded.data(), encoded.size()) != 1)
          throw std::runtime_error("Cannot fingerprint change-feed row");
      }
      if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db_.handle()));
    }
    db_.exec("COMMIT");
  } catch (...) {
    db_.exec("ROLLBACK");
    throw;
  }
  for (auto& [key, digest] : digests) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(digest.get(), bytes.data(), &size) != 1)
      throw std::runtime_error("Cannot finalize change-feed fingerprint");
    result.emplace(key, std::string(reinterpret_cast<const char*>(bytes.data()), size));
  }
  return result;
}

void ChangeFeed::poll() {
  const auto current_version = data_version();
  auto current_revisions = read_revisions();
  if (current_version == version_ && current_revisions == revisions_) return;
  auto current = current_version == version_ ? previous_ : snapshot();
  std::set<Key> changed;
  for (const auto& [key, fingerprint] : current) {
    const auto previous = previous_.find(key);
    if (previous == previous_.end() || previous->second != fingerprint) changed.insert(key);
  }
  for (const auto& [key, fingerprint] : previous_) {
    if (!current.contains(key)) changed.insert(key);
  }
  for (const auto& [project, revision] : current_revisions.items()) {
    if (!revisions_.contains(project) || revisions_[project] != revision)
      changed.insert(Key{"project", project, project});
  }
  for (const auto& [kind, id, project] : changed) {
    journal_.append(
        kind + ".changed",
        {{"entity", kind},
         {"entity_id", id},
         {"project_id", project.empty() ? nlohmann::json(nullptr) : nlohmann::json(project)},
         {"deleted", !current.contains(Key{kind, id, project})},
         {"git_revision", current_revisions.value(project, nlohmann::json(nullptr))}}
    );
  }
  previous_ = std::move(current);
  version_ = current_version;
  revisions_ = std::move(current_revisions);
}

} // namespace holder::api::support
