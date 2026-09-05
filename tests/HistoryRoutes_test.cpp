#include "api/routes/HistoryRoutes.h"
#include "http_test_helpers.h"

#include "card/CardFrontMatter.h"
#include "card/CardPaths.h"
#include "git/GitRepo.h"
#include "project/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

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

  query["from"] = *old_oid;
  query["to"] = list["head_oid"].get<std::string>();
  res = {};
  REQUIRE(holder::api::routes::handle_history_routes(base + "/compare", req, res, db, param));
  REQUIRE(res.result() == http::status::ok);
  const auto comparison = nlohmann::json::parse(res.body())["data"];
  CHECK(comparison["from"]["body"] == "Old body\n");
  CHECK(comparison["to"]["body"] == "New body\n");

  query["from"] = "not-an-oid";
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
