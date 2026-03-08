#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Card.h"
#include "model/Project.h"
#include "card/CardRepo.h"
#include "platform/Db.h"
#include "project/ProjectRepo.h"

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
  auto dir = base / ("holder_card_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

void apply_schema(holder::platform::Db& db) {
  const auto schema_path = find_schema_sql();
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);
}

void create_project(holder::platform::Db& db, const std::string& project_id) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

} // namespace

TEST_CASE("CardRepo CRUD", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = "proj-1";
  card.title = "First";
  card.rel_path = "cards/ca/rd/card-1.md";
  card.sort_key = 1.0;
  card.created_at = 10;
  card.updated_at = 10;

  repo.create(card);

  const auto fetched = repo.get("card-1");
  REQUIRE(fetched.has_value());
  REQUIRE(fetched->title == "First");
  REQUIRE(fetched->parent_card_id.has_value() == false);

  auto list_root = repo.list_roots("proj-1");
  REQUIRE(list_root.size() == 1);

  holder::model::Card child;
  child.card_id = "card-2";
  child.project_id = "proj-1";
  child.title = "Child";
  child.rel_path = "cards/ca/rd/card-2.md";
  child.parent_card_id = "card-1";
  child.sort_key = 2.0;
  child.created_at = 11;
  child.updated_at = 11;
  repo.create(child);

  list_root = repo.list_roots("proj-1");
  REQUIRE(list_root.size() == 1);

  auto list_child = repo.list_children("proj-1", "card-1");
  REQUIRE(list_child.size() == 1);
  REQUIRE(list_child[0].card_id == "card-2");

  repo.update_title("card-1", "Renamed", 20);
  const auto renamed = repo.get("card-1");
  REQUIRE(renamed.has_value());
  REQUIRE(renamed->title == "Renamed");
  REQUIRE(renamed->updated_at == 20);

  repo.move("card-2", std::nullopt, 3.0, 30);
  list_root = repo.list_roots("proj-1");
  REQUIRE(list_root.size() == 2);

  repo.soft_delete("card-2", 40, 41);
  const auto deleted = repo.get("card-2");
  REQUIRE(deleted.has_value());
  REQUIRE(deleted->deleted_at.has_value());
  REQUIRE(deleted->deleted_at.value() == 40);
  REQUIRE(deleted->updated_at == 41);
}

TEST_CASE("CardRepo counts children and handles deleted_at on create", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);

  holder::model::Card parent;
  parent.card_id = "parent-1";
  parent.project_id = "proj-1";
  parent.title = "Parent";
  parent.rel_path = "cards/pa/re/parent-1.md";
  parent.sort_key = 1.0;
  parent.created_at = 1;
  parent.updated_at = 1;
  repo.create(parent);

  holder::model::Card child_visible;
  child_visible.card_id = "child-1";
  child_visible.project_id = "proj-1";
  child_visible.title = "Child 1";
  child_visible.rel_path = "cards/ch/il/child-1.md";
  child_visible.parent_card_id = parent.card_id;
  child_visible.sort_key = 2.0;
  child_visible.created_at = 2;
  child_visible.updated_at = 2;
  repo.create(child_visible);

  holder::model::Card child_deleted;
  child_deleted.card_id = "child-2";
  child_deleted.project_id = "proj-1";
  child_deleted.title = "Child 2";
  child_deleted.rel_path = "cards/ch/il/child-2.md";
  child_deleted.parent_card_id = parent.card_id;
  child_deleted.sort_key = 3.0;
  child_deleted.created_at = 3;
  child_deleted.updated_at = 3;
  child_deleted.deleted_at = 99; // exercises bind_int64_optional(value.has_value())
  repo.create(child_deleted);

  REQUIRE(repo.count_children_not_deleted("proj-1", parent.card_id) == 1);
}

TEST_CASE("CardRepo throws sqlite error when DB handle is invalid", "[cardrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");

  holder::card::CardRepo repo(db);
  db.close();

  holder::model::Card card;
  card.card_id = "invalid-db";
  card.project_id = "proj-1";
  card.title = "Broken";
  card.rel_path = "cards/in/va/invalid-db.md";
  card.sort_key = 1.0;
  card.created_at = 1;
  card.updated_at = 1;

  REQUIRE_THROWS(repo.create(card));
}
