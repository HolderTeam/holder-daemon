#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/CardPaths.h"
#include "core/CardFrontMatter.h"
#include "git/GitRepo.h"
#include "model/Card.h"
#include "model/Project.h"
#include "store/CardRepo.h"
#include "store/CardStore.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"
#include "index/FtsIndexer.h"

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

void create_project(holder::store::Db& db,
                    const std::string& project_id,
                    const std::string& root_path) {
  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path;
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
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd1234";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "hello");

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = project_root / rel_path;
  REQUIRE(std::filesystem::exists(full_path));
  const auto raw = read_file(full_path);
  REQUIRE(raw.find("---\n") == 0);
  REQUIRE(raw.find("card_id: abcd1234") != std::string::npos);
  REQUIRE(raw.find("project_id: proj-1") != std::string::npos);
  REQUIRE(raw.rfind("hello") != std::string::npos);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.card_id == "abcd1234");
  REQUIRE(parsed.card.project_id == "proj-1");
  REQUIRE(parsed.card.title == "First");
  REQUIRE(parsed.body == "hello");

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
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd5678";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "hello");
  store.update_content(card.card_id, "updated", std::optional<std::string>("Renamed"), 20);

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = project_root / rel_path;
  REQUIRE(std::filesystem::exists(full_path));
  const auto raw = read_file(full_path);
  REQUIRE(raw.find("---\n") == 0);
  REQUIRE(raw.find("title: Renamed") != std::string::npos);
  REQUIRE(raw.rfind("updated") != std::string::npos);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.title == "Renamed");
  REQUIRE(parsed.card.updated_at == 20);
  REQUIRE(parsed.body == "updated");

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
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd9999";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "same");
  const int before = count_commits(project_root);

  store.update_content(card.card_id, "same", std::nullopt, 20);
  const int after = count_commits(project_root);

  REQUIRE(before == after);

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  const auto full_path = project_root / rel_path;
  const auto raw = read_file(full_path);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.title == "First");
  REQUIRE(parsed.card.updated_at == 10);
  REQUIRE(parsed.body == "same");
}

TEST_CASE("CardStore update creates commit when content changes", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcf0000";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "one");
  const int before = count_commits(project_root);

  store.update_content(card.card_id, "two", std::nullopt, 20);
  const int after = count_commits(project_root);

  REQUIRE(after == before + 1);
}

TEST_CASE("CardStore move updates parent and sort metadata", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);

  holder::model::Card parent;
  parent.card_id = "parent01";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.created_at = 10;
  parent.updated_at = 10;
  store.create(parent, "parent body");

  holder::model::Card child;
  child.card_id = "child001";
  child.project_id = "proj-1";
  child.title = "Child";
  child.created_at = 11;
  child.updated_at = 11;
  store.create(child, "child body");

  const int before = count_commits(project_root);
  store.move(child.card_id, true, std::optional<std::string>(parent.card_id), std::optional<double>(42.5), 20);
  const int after = count_commits(project_root);
  REQUIRE(after == before + 1);

  holder::store::CardRepo card_repo(db);
  const auto moved = card_repo.get(child.card_id);
  REQUIRE(moved.has_value());
  REQUIRE(moved->parent_card_id.has_value());
  REQUIRE(moved->parent_card_id.value() == parent.card_id);
  REQUIRE(moved->sort_key == 42.5);
  REQUIRE(moved->updated_at == 20);

  const auto rel_path = holder::core::card_rel_path(child.card_id);
  const auto full_path = project_root / rel_path;
  const auto raw = read_file(full_path);
  const auto parsed = holder::core::parse_card_file(raw);
  REQUIRE(parsed.has_front_matter);
  REQUIRE(parsed.card.parent_card_id.has_value());
  REQUIRE(parsed.card.parent_card_id.value() == parent.card_id);
  REQUIRE(parsed.card.sort_key == 42.5);
  REQUIRE(parsed.card.updated_at == 20);
  REQUIRE(parsed.body == "child body");
}

TEST_CASE("CardStore create rejects duplicate card_id", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abcd1111";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "one");
  REQUIRE_THROWS(store.create(card, "two"));
}

TEST_CASE("CardStore create rejects existing file without DB row", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);
  holder::model::Card card;
  card.card_id = "abca2222";
  card.project_id = "proj-1";
  card.title = "First";
  card.created_at = 10;
  card.updated_at = 10;

  const auto rel_path = holder::core::card_rel_path(card.card_id);
  holder::git::GitRepo repo;
  repo.open_or_init(project_root);
  repo.write_file(rel_path, "manual");

  REQUIRE_THROWS(store.create(card, "one"));
}

TEST_CASE("CardStore create appends to end of sibling scope when sort omitted", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);

  holder::model::Card root_a;
  root_a.card_id = "root-a";
  root_a.project_id = "proj-1";
  root_a.title = "Root A";
  root_a.created_at = 10;
  root_a.updated_at = 10;
  store.create(root_a, "a");

  holder::model::Card root_b;
  root_b.card_id = "root-b";
  root_b.project_id = "proj-1";
  root_b.title = "Root B";
  root_b.created_at = 11;
  root_b.updated_at = 11;
  store.create(root_b, "b");

  holder::model::Card parent;
  parent.card_id = "parent-a";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.created_at = 12;
  parent.updated_at = 12;
  store.create(parent, "p");

  holder::model::Card child_a;
  child_a.card_id = "child-a";
  child_a.project_id = "proj-1";
  child_a.parent_card_id = parent.card_id;
  child_a.title = "Child A";
  child_a.created_at = 13;
  child_a.updated_at = 13;
  store.create(child_a, "ca");

  holder::model::Card child_b;
  child_b.card_id = "child-b";
  child_b.project_id = "proj-1";
  child_b.parent_card_id = parent.card_id;
  child_b.title = "Child B";
  child_b.created_at = 14;
  child_b.updated_at = 14;
  store.create(child_b, "cb");

  holder::store::CardRepo card_repo(db);
  const auto got_root_a = card_repo.get("root-a");
  const auto got_root_b = card_repo.get("root-b");
  const auto got_parent = card_repo.get("parent-a");
  const auto got_child_a = card_repo.get("child-a");
  const auto got_child_b = card_repo.get("child-b");
  REQUIRE(got_root_a.has_value());
  REQUIRE(got_root_b.has_value());
  REQUIRE(got_parent.has_value());
  REQUIRE(got_child_a.has_value());
  REQUIRE(got_child_b.has_value());

  REQUIRE(got_root_a->sort_key == 0.0);
  REQUIRE(got_root_b->sort_key == 1.0);
  REQUIRE(got_parent->sort_key == 2.0);
  REQUIRE(got_child_a->sort_key == 0.0);
  REQUIRE(got_child_b->sort_key == 1.0);
}

TEST_CASE("CardStore create preserves explicit sort_key", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);

  holder::model::Card card;
  card.card_id = "explicit-1";
  card.project_id = "proj-1";
  card.title = "Explicit";
  card.created_at = 10;
  card.updated_at = 10;

  store.create(card, "body", std::optional<double>(42.5));

  holder::store::CardRepo card_repo(db);
  const auto fetched = card_repo.get(card.card_id);
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->sort_key == 42.5);
}

TEST_CASE("CardStore move across parent appends when sort omitted", "[cardstore]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore store(db, &fts);

  holder::model::Card parent_a;
  parent_a.card_id = "parent-a";
  parent_a.project_id = "proj-1";
  parent_a.title = "Parent A";
  parent_a.created_at = 10;
  parent_a.updated_at = 10;
  store.create(parent_a, "pa");

  holder::model::Card parent_b;
  parent_b.card_id = "parent-b";
  parent_b.project_id = "proj-1";
  parent_b.title = "Parent B";
  parent_b.created_at = 11;
  parent_b.updated_at = 11;
  store.create(parent_b, "pb");

  holder::model::Card b_child_1;
  b_child_1.card_id = "b-child-1";
  b_child_1.project_id = "proj-1";
  b_child_1.parent_card_id = parent_b.card_id;
  b_child_1.title = "B Child 1";
  b_child_1.created_at = 12;
  b_child_1.updated_at = 12;
  store.create(b_child_1, "b1");

  holder::model::Card moving;
  moving.card_id = "moving-1";
  moving.project_id = "proj-1";
  moving.parent_card_id = parent_a.card_id;
  moving.title = "Moving";
  moving.created_at = 13;
  moving.updated_at = 13;
  store.create(moving, "m1");

  store.move(moving.card_id, true, std::optional<std::string>(parent_b.card_id), std::nullopt, 20);

  holder::store::CardRepo card_repo(db);
  const auto moved = card_repo.get(moving.card_id);
  REQUIRE(moved.has_value());
  REQUIRE(moved->parent_card_id.has_value());
  REQUIRE(moved->parent_card_id.value() == parent_b.card_id);
  REQUIRE(moved->sort_key == 1.0);
}
