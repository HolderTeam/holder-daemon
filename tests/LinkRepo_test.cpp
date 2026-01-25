#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "model/Card.h"
#include "model/CardLink.h"
#include "model/Project.h"
#include "store/CardRepo.h"
#include "store/Db.h"
#include "store/LinkRepo.h"
#include "store/ProjectRepo.h"

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
  auto dir = base / ("holder_link_test_" + suffix);
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

void create_card(holder::store::Db& db,
                 const std::string& card_id,
                 const std::string& project_id) {
  holder::store::CardRepo repo(db);
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = project_id;
  card.title = card_id;
  card.rel_path = "cards/" + card_id + ".md";
  card.sort_key = 0.0;
  card.created_at = 1;
  card.updated_at = 1;
  repo.create(card);
}

} // namespace

TEST_CASE("LinkRepo upsert/list/delete", "[linkrepo]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);
  apply_schema(db);
  create_project(db, "proj-1");
  create_card(db, "card-a", "proj-1");
  create_card(db, "card-b", "proj-1");
  create_card(db, "card-c", "proj-1");

  holder::store::LinkRepo repo(db);

  holder::model::CardLink link1;
  link1.project_id = "proj-1";
  link1.from_card_id = "card-a";
  link1.to_card_id = "card-b";
  link1.to_type = "card";
  link1.kind = "wiki";
  link1.label = "Label";
  link1.created_at = 10;

  holder::model::CardLink link2;
  link2.project_id = "proj-1";
  link2.from_card_id = "card-a";
  link2.to_card_id = "res-1";
  link2.to_type = "resource";
  link2.kind = "ref";
  link2.created_at = 11;

  repo.upsert_links("proj-1", "card-a", {link1, link2});

  auto outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.size() == 2);

  auto backlinks = repo.list_backlinks("proj-1", "card-b");
  REQUIRE(backlinks.size() == 1);
  REQUIRE(backlinks[0].from_card_id == "card-a");
  REQUIRE(backlinks[0].label.has_value());

  link1.label = "New Label";
  link1.created_at = 12;
  repo.upsert_links("proj-1", "card-a", {link1});

  outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.size() == 2);

  repo.delete_links_from("proj-1", "card-a");
  outgoing = repo.list_outgoing("proj-1", "card-a");
  REQUIRE(outgoing.empty());
}
