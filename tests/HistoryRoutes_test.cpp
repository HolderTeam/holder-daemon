#include "api/routes/HistoryRoutes.h"
#include "http_test_helpers.h"

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "git/GitRepo.h"
#include "project/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>

namespace http = boost::beast::http;

namespace {

std::string history_card_file(const std::string& card_id, const std::string& body) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = "history-project";
  card.title = "History card";
  card.rel_path = holder::core::card_rel_path(card_id);
  card.created_at = 1;
  card.updated_at = 2;
  return holder::core::render_card_front_matter(card, {}, {}) + body;
}

void history_commit(
    holder::git::GitRepo& git,
    const std::string& card_id,
    const std::string& body,
    const std::string& message
) {
  const auto path = holder::core::card_rel_path(card_id);
  git.write_file(path, history_card_file(card_id, body));
  git.stage_path(path);
  git.commit(message);
}

struct FileSnapshot {
  bool exists = false;
  std::string bytes;
};

FileSnapshot snapshot_file(const std::filesystem::path& path) {
  FileSnapshot snapshot;
  snapshot.exists = std::filesystem::exists(path);
  if (!snapshot.exists) return snapshot;
  std::ifstream input(path, std::ios::binary);
  snapshot.bytes.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  return snapshot;
}

void check_file_unchanged(const std::filesystem::path& path, const FileSnapshot& before) {
  const auto after = snapshot_file(path);
  CHECK(after.exists == before.exists);
  CHECK(after.bytes == before.bytes);
}

} // namespace

TEST_CASE("HistoryRoutes lists and compares card versions", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());

  const std::string card_id = "abcd-route-history";
  holder::git::GitRepo git;
  git.open_or_init(project_root);
  history_commit(git, card_id, "Old body\n", "Add card History card");
  const auto old_oid = git.head_oid();
  REQUIRE(old_oid.has_value());
  history_commit(git, card_id, "New body\n", "Update card History card");

  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  std::unordered_map<std::string, std::string> query;
  auto param = [&](const std::string& key) {
    const auto found = query.find(key);
    return found == query.end() ? std::string{} : found->second;
  };
  const auto base = "/projects/history-project/history/cards/" + card_id;
  REQUIRE(holder::api::routes::handle_history_routes(base, req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto list = nlohmann::json::parse(res.body())["data"];
  REQUIRE(list["entries"].size() == 2);
  REQUIRE(list["head_oid"].is_string());
  CHECK_FALSE(list["scan_limited"].get<bool>());
  REQUIRE(list["entries"][0]["saves"].size() == 1);
  REQUIRE(list["entries"][0]["visible_parent_oids"].is_array());
  REQUIRE(list["entries"][0]["visible_parent_oids"].size() == 1);
  CHECK(list["entries"][0]["visible_parent_oids"][0] == list["entries"][1]["last_oid"]);
  CHECK(list["entries"][0]["saves"][0]["oid"] == list["entries"][0]["last_oid"]);
  CHECK(list["entries"][0]["saves"][0]["parent_oids"].is_array());
  CHECK(list["entries"][0]["saves"][0]["message"] == "Update card History card");
  CHECK(list["entries"][0]["saves"][0]["authored_at"].is_number_integer());

  query["from"] = *old_oid;
  query["to"] = list["head_oid"].get<std::string>();
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto comparison = nlohmann::json::parse(res.body())["data"];
  CHECK(comparison["from"]["body"] == "Old body\n");
  CHECK(comparison["to"]["body"] == "New body\n");

  query.clear();
  query["to"] = *old_oid;
  query["mode"] = "change";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto creation = nlohmann::json::parse(res.body())["data"];
  CHECK_FALSE(creation["from"]["exists"].get<bool>());
  CHECK(creation["to"]["body"] == "Old body\n");

  query["mode"] = "unsupported";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  CHECK(res.result() == http::status::bad_request);

  query["mode"] = "since";
  query["from"] = "not-an-oid";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  CHECK(res.result() == http::status::bad_request);

  query.clear();
  query["to"] = list["head_oid"].get<std::string>();
  query["mode"] = "since";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  CHECK(res.result() == http::status::bad_request);

  query.clear();
  query["mode"] = "change";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  CHECK(res.result() == http::status::bad_request);

  query.clear();
  query["cursor"] = "not-an-oid";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base, req, res, db, param));
  CHECK(res.result() == http::status::bad_request);

  query.clear();
  query["limit"] = "999999999999999999999999999999999999";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base, req, res, db, param));
  CHECK(res.result() == http::status::bad_request);
}

TEST_CASE("HistoryRoutes lists and filters project activities", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());

  holder::git::GitRepo git;
  git.open_or_init(project_root);
  git.write_file(
      "cards/ab/cd/abcd-project-route.md",
      history_card_file("abcd-project-route", "Project history card")
  );
  git.write_file("resources/ef/gh/efgh-project-route.json", "resource");
  git.stage_paths({
      "cards/ab/cd/abcd-project-route.md", "resources/ef/gh/efgh-project-route.json"
  });
  git.commit("Attach project resource");
  git.write_file("notes/from-another-tool.txt", "external");
  git.stage_path("notes/from-another-tool.txt");
  git.commit("External project note");

  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  std::unordered_map<std::string, std::string> query;
  auto param = [&](const std::string& key) {
    const auto found = query.find(key);
    return found == query.end() ? std::string{} : found->second;
  };
  const std::string path = "/projects/history-project/history";

  REQUIRE(holder::api::routes::handle_history_routes(path, req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto page = nlohmann::json::parse(res.body())["data"];
  REQUIRE(page["activities"].size() == 2);
  CHECK(page["activities"][0]["message"] == "External project note");
  REQUIRE(page["activities"][1]["affected_objects"].size() == 2);
  CHECK(page["activities"][1]["affected_objects"][0]["kind"] == "card");
  CHECK(page["activities"][1]["affected_objects"][0]["items"][0]["path"] ==
        "cards/ab/cd/abcd-project-route.md");
  CHECK(page["activities"][1]["affected_objects"][0]["items"][0]["title"] ==
        "History card");
  CHECK(page["activities"][1]["affected_objects"][1]["kind"] == "resource");

  query["kind"] = "resource";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(path, req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto resource_page = nlohmann::json::parse(res.body())["data"];
  REQUIRE(resource_page["activities"].size() == 1);
  CHECK(resource_page["activities"][0]["message"] == "Attach project resource");

  query["kind"] = "not-a-kind";
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(path, req, res, db, param));
  CHECK(res.result() == http::status::bad_request);
}

TEST_CASE("HistoryRoutes validates project and comparison parameters", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  auto empty_param = [](const std::string&) { return std::string{}; };

  REQUIRE(holder::api::routes::handle_history_routes(
      "/projects/missing/history/cards/abcd-card", req, res, db, empty_param
  ));
  CHECK(res.result() == http::status::not_found);
  CHECK_FALSE(holder::api::routes::handle_history_routes(
      "/cards/abcd-card/history", req, res, db, empty_param
  ));

  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(
      "/projects/history-project/history/cards/abc", req, res, db, empty_param
  ));
  CHECK(res.result() == http::status::bad_request);
}

TEST_CASE("HistoryRoutes handles an empty card history and card-absent revision", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());

  holder::git::GitRepo git;
  git.open_or_init(project_root);
  git.write_file("project-notes.md", "This repository revision has no card.\n");
  git.stage_path("project-notes.md");
  git.commit("Add project note");
  const auto unrelated_oid = git.head_oid();
  REQUIRE(unrelated_oid.has_value());

  const std::string card_id = "abcd-no-history";
  const auto base = "/projects/history-project/history/cards/" + card_id;
  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  std::unordered_map<std::string, std::string> query;
  auto param = [&](const std::string& key) {
    const auto found = query.find(key);
    return found == query.end() ? std::string{} : found->second;
  };

  REQUIRE(holder::api::routes::handle_history_routes(base, req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto list = nlohmann::json::parse(res.body())["data"];
  CHECK(list["head_oid"] == *unrelated_oid);
  CHECK(list["entries"].empty());
  CHECK(list["next_cursor"].is_null());

  query["from"] = *unrelated_oid;
  query["to"] = *unrelated_oid;
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto comparison = nlohmann::json::parse(res.body())["data"];
  CHECK_FALSE(comparison["from"]["exists"].get<bool>());
  CHECK_FALSE(comparison["to"]["exists"].get<bool>());
  CHECK(comparison["lines"].empty());
}

TEST_CASE("HistoryRoutes compares captured revisions after a later autosave", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());

  const std::string card_id = "abcd-captured-history";
  holder::git::GitRepo git;
  git.open_or_init(project_root);
  history_commit(git, card_id, "First saved body\n", "Add card History card");
  const auto first_oid = git.head_oid();
  REQUIRE(first_oid.has_value());
  history_commit(git, card_id, "Captured saved body\n", "Update card History card");

  const auto base = "/projects/history-project/history/cards/" + card_id;
  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  std::unordered_map<std::string, std::string> query;
  auto param = [&](const std::string& key) {
    const auto found = query.find(key);
    return found == query.end() ? std::string{} : found->second;
  };

  REQUIRE(holder::api::routes::handle_history_routes(base, req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto captured_oid = nlohmann::json::parse(res.body())["data"]["head_oid"].get<std::string>();

  history_commit(git, card_id, "Later autosave body\n", "Update card History card");

  query["from"] = *first_oid;
  query["to"] = captured_oid;
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto comparison = nlohmann::json::parse(res.body())["data"];
  CHECK(comparison["from"]["oid"] == *first_oid);
  CHECK(comparison["from"]["body"] == "First saved body\n");
  CHECK(comparison["to"]["oid"] == captured_oid);
  CHECK(comparison["to"]["body"] == "Captured saved body\n");
}

TEST_CASE("HistoryRoutes rejects an oversized history comparison", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());

  const std::string card_id = "abcd-oversized-history";
  holder::git::GitRepo git;
  git.open_or_init(project_root);
  history_commit(git, card_id, "Small version\n", "Add card History card");
  const auto old_oid = git.head_oid();
  REQUIRE(old_oid.has_value());
  const std::string oversized(2 * 1024 * 1024, 'x');
  history_commit(git, card_id, oversized, "Update card History card");
  const auto head_oid = git.head_oid();
  REQUIRE(head_oid.has_value());

  const auto base = "/projects/history-project/history/cards/" + card_id;
  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  std::unordered_map<std::string, std::string> query;
  auto param = [&](const std::string& key) {
    const auto found = query.find(key);
    return found == query.end() ? std::string{} : found->second;
  };

  query["from"] = *old_oid;
  query["to"] = *head_oid;
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  REQUIRE(res.result() == http::status::payload_too_large);
  CHECK(nlohmann::json::parse(res.body())["error"]["code"] == "history_response_too_large");
}

TEST_CASE("HistoryRoutes leave SQLite files unchanged", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = holder::test::open_db_with_schema(db_path);
  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());

  const std::string card_id = "abcd-sqlite-history";
  holder::git::GitRepo git;
  git.open_or_init(project_root);
  history_commit(git, card_id, "First body\n", "Add card History card");
  const auto old_oid = git.head_oid();
  REQUIRE(old_oid.has_value());
  history_commit(git, card_id, "Second body\n", "Update card History card");
  const auto head_oid = git.head_oid();
  REQUIRE(head_oid.has_value());

  const auto db_before = snapshot_file(db_path);
  const auto wal_before = snapshot_file(db_path.string() + "-wal");
  const auto journal_before = snapshot_file(db_path.string() + "-journal");

  const auto base = "/projects/history-project/history/cards/" + card_id;
  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  std::unordered_map<std::string, std::string> query;
  auto param = [&](const std::string& key) {
    const auto found = query.find(key);
    return found == query.end() ? std::string{} : found->second;
  };

  REQUIRE(holder::api::routes::handle_history_routes(base, req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  query["from"] = *old_oid;
  query["to"] = *head_oid;
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  REQUIRE(res.result() == http::status::ok);

  // SQLite can update the -shm file solely to record an active WAL reader.
  // That transient lock bookkeeping is not persistent database state, so this
  // proof deliberately checks the database and durable journal files instead.
  check_file_unchanged(db_path, db_before);
  check_file_unchanged(db_path.string() + "-wal", wal_before);
  check_file_unchanged(db_path.string() + "-journal", journal_before);
}

TEST_CASE("HistoryRoutes reports an unavailable encrypted project key", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "encrypted-project";
  holder::model::Project project;
  project.project_id = "encrypted-history-project";
  project.name = "Encrypted history";
  project.root_path = project_root.string();
  project.privacy_mode = "encrypted_git";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  holder::project::ProjectRepo(db).create(project);

  const std::string card_id = "abcd-encrypted-route";
  holder::git::GitRepo git;
  git.open_or_init(project_root);
  history_commit(git, card_id, "Encrypted body\n", "Add card Encrypted history");

  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  auto empty_param = [](const std::string&) { return std::string{}; };
  REQUIRE(holder::api::routes::handle_history_routes(
      "/projects/encrypted-history-project/history/cards/" + card_id,
      req,
      res,
      db,
      empty_param
  ));
  CHECK(res.result() == http::status::conflict);
  const auto error = nlohmann::json::parse(res.body())["error"];
  CHECK(error["code"] == "history_key_unavailable");
}

TEST_CASE("HistoryRoutes reports malformed historical card data", "[http][history]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "project";
  holder::test::create_project(db, "history-project", project_root.string());

  const std::string card_id = "abcd-malformed-route";
  holder::git::GitRepo git;
  git.open_or_init(project_root);
  const auto path = holder::core::card_rel_path(card_id);
  git.write_file(path, "Not a Holder card file\n");
  git.stage_path(path);
  git.commit("Update card History card");

  http::request<http::string_body> req{http::verb::get, "/", 11};
  http::response<http::string_body> res;
  auto empty_param = [](const std::string&) { return std::string{}; };
  REQUIRE(holder::api::routes::handle_history_routes(
      "/projects/history-project/history/cards/" + card_id,
      req,
      res,
      db,
      empty_param
  ));
  CHECK(res.result() == http::status::service_unavailable);
  const auto error = nlohmann::json::parse(res.body())["error"];
  CHECK(error["code"] == "history_unavailable");
  CHECK(error["message"] == "Historical card content is malformed");
}
