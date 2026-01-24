#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/CardPaths.h"
#include "git/GitRepo.h"
#include "model/Card.h"
#include "model/Project.h"
#include "store/CardRepo.h"
#include "store/CardStore.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"

#include <git2.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path find_schema_sql() {
#ifdef SCHEMA_SQL_PATH
  std::filesystem::path p = SCHEMA_SQL_PATH;
  if (std::filesystem::exists(p)) return p;
#endif
  namespace fs = std::filesystem;
  fs::path p1 = fs::current_path() / "schema" / "schema.sql";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "schema" / "schema.sql";
  if (fs::exists(p2)) return p2;
  throw std::runtime_error("schema.sql not found for tests");
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_card_store_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::store::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(holder::store::Db& db, const std::string& project_id) {
  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return data;
}

int count_commits(const std::filesystem::path& repo_dir) {
  git_repository* repo = nullptr;
  if (git_repository_open(&repo, repo_dir.string().c_str()) != 0) {
    return -1;
  }

  git_revwalk* walk = nullptr;
  if (git_revwalk_new(&walk, repo) != 0) {
    git_repository_free(repo);
    return -1;
  }

  int count = 0;
  if (git_revwalk_push_head(walk) == 0) {
    git_oid oid{};
    while (git_revwalk_next(&oid, walk) == 0) {
      ++count;
    }
  }

  git_revwalk_free(walk);
  git_repository_free(repo);
  return count;
}

} // namespace

TEST_CASE("CardStore create writes file and DB", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::store::CardStore store(db, repo);
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "hello");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = repo_dir / rel_path;
  REQUIRE(std::filesystem::exists(full_path));
  REQUIRE(read_file(full_path) == "hello");

  holder::store::CardRepo card_repo(db);
  const auto fetched = card_repo.get("abcd1234");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->rel_path == rel_path);
}

TEST_CASE("CardStore update writes file and updates metadata", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::store::CardStore store(db, repo);
  holder::model::Card card;
  card.card_id = "abcd5678";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "hello");
  store.update_content(card.card_id, "updated", std::optional<std::string>("Renamed"), 20);

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = repo_dir / rel_path;
  REQUIRE(std::filesystem::exists(full_path));
  REQUIRE(read_file(full_path) == "updated");

  holder::store::CardRepo card_repo(db);
  const auto fetched = card_repo.get(card.card_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->title == "Renamed");
  REQUIRE(fetched->updated_at == 20);
}

TEST_CASE("CardStore update skips commit when content unchanged", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::store::CardStore store(db, repo);
  holder::model::Card card;
  card.card_id = "abcd9999";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "same");
  const int before = count_commits(repo_dir);

  store.update_content(card.card_id, "same", std::nullopt, 20);
  const int after = count_commits(repo_dir);

  REQUIRE(before == after);
}
