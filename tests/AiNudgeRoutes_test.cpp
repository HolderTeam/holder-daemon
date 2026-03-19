#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiNudgeRoutes.h"
#include "ai/NudgeService.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {

namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target, const std::string& body) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  req.body() = body;
  req.prepare_payload();
  return req;
}

void create_card_fixture(holder::platform::Db& db,
                         const std::string& project_id,
                         const std::string& card_id) {
  db.exec(
      "INSERT INTO cards(card_id, project_id, title, rel_path, created_at, updated_at) "
      "VALUES('" + card_id + "', '" + project_id + "', 'Fixture', 'cards/" + card_id + ".md', 1, 1);");
}

} // namespace

TEST_CASE("AiNudgeRoutes evaluates known nudge candidates", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1");
  create_card_fixture(db, "proj-1", "card-1");
  holder::ai::NudgeService service(db);

  SECTION("card title only candidate can be accepted for nudging") {
    auto req = make_request(http::verb::post,
                            "/ai/nudges/evaluate",
                            R"({
                              "kind":"card.title_only",
                              "project_id":"proj-1",
                              "card_id":"card-1",
                              "created_at":123,
                              "basis_fingerprint":"abc123",
                              "facts":{
                                "title":"Frog",
                                "body_empty":true,
                                "doc_chars":12,
                                "body_chars":0
                              }
                            })");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["kind"] == "card.title_only");
    REQUIRE(payload["data"]["accepted"] == true);
    REQUIRE(payload["data"]["should_nudge"] == true);
    REQUIRE(payload["data"]["reason"] == "title_only_candidate_ready");
    REQUIRE(payload["data"]["nudge"]["kind"] == "card.title_only");
    REQUIRE(payload["data"]["nudge"]["title"] == "Start this card");
  }

  SECTION("card stuck drafting candidate can be accepted for nudging") {
    auto req = make_request(http::verb::post,
                            "/ai/nudges/evaluate",
                            R"({
                              "kind":"card.stuck_drafting",
                              "project_id":"proj-1",
                              "card_id":"card-1",
                              "created_at":123,
                              "basis_fingerprint":"abc123",
                              "facts":{
                                "title":"Frog",
                                "autosave_count":4,
                                "doc_chars":72,
                                "body_chars":48,
                                "duration_seconds":180
                              }
                            })");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["kind"] == "card.stuck_drafting");
    REQUIRE(payload["data"]["accepted"] == true);
    REQUIRE(payload["data"]["should_nudge"] == true);
    REQUIRE(payload["data"]["reason"] == "stuck_drafting_candidate_ready");
  }

  SECTION("repeated failed push candidate can be accepted for nudging") {
    auto req = make_request(http::verb::post,
                            "/ai/nudges/evaluate",
                            R"({
                              "kind":"git.push_failed_repeated",
                              "project_id":"proj-1",
                              "created_at":123,
                              "basis_commit":"deadbeef",
                              "facts":{
                                "failure_count":3,
                                "latest_status":"auth_failed",
                                "branch":"main"
                              }
                            })");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["accepted"] == true);
    REQUIRE(payload["data"]["should_nudge"] == true);
    REQUIRE(payload["data"]["reason"] == "git_push_failure_candidate_ready");
  }
}

TEST_CASE("AiNudgeRoutes rejects malformed or non-actionable candidates", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1");
  create_card_fixture(db, "proj-1", "card-1");
  holder::ai::NudgeService service(db);

  SECTION("unrelated path returns false") {
    auto req = make_request(http::verb::post, "/ai/nudges/nope", "{}");
    http::response<http::string_body> res;

    REQUIRE_FALSE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/nope", req, res, &service));
  }

  SECTION("wrong method returns false") {
    auto req = make_request(http::verb::get, "/ai/nudges/evaluate", "");
    http::response<http::string_body> res;

    REQUIRE_FALSE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
  }

  SECTION("invalid json returns bad request") {
    auto req = make_request(http::verb::post, "/ai/nudges/evaluate", "{");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::bad_request);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "invalid_json");
  }

  SECTION("invalid body returns bad request") {
    auto req = make_request(http::verb::post, "/ai/nudges/evaluate", R"({"kind":"card.title_only"})");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::bad_request);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "invalid_body");
  }

  SECTION("empty kind or project_id returns bad request") {
    auto req = make_request(http::verb::post,
                            "/ai/nudges/evaluate",
                            R"({
                              "kind":"",
                              "project_id":"",
                              "created_at":123,
                              "facts":{}
                            })");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::bad_request);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "invalid_body");
    REQUIRE(payload["error"]["message"] == "kind and project_id are required.");
  }

  SECTION("placeholder title only candidate is accepted but not actionable") {
    auto req = make_request(http::verb::post,
                            "/ai/nudges/evaluate",
                            R"({
                              "kind":"card.title_only",
                              "project_id":"proj-1",
                              "card_id":"card-1",
                              "created_at":123,
                              "facts":{
                                "title":"Untitled child of Cats",
                                "body_empty":true,
                                "doc_chars":24,
                                "body_chars":0
                              }
                            })");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["accepted"] == true);
    REQUIRE(payload["data"]["should_nudge"] == false);
    REQUIRE(payload["data"]["reason"] == "title_only_not_actionable");
  }

  SECTION("insufficient stuck drafting evidence is not actionable") {
    auto req = make_request(http::verb::post,
                            "/ai/nudges/evaluate",
                            R"({
                              "kind":"card.stuck_drafting",
                              "project_id":"proj-1",
                              "card_id":"card-1",
                              "created_at":123,
                              "facts":{
                                "title":"Frog",
                                "autosave_count":2,
                                "doc_chars":32,
                                "body_chars":24,
                                "duration_seconds":40
                              }
                            })");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["accepted"] == true);
    REQUIRE(payload["data"]["should_nudge"] == false);
    REQUIRE(payload["data"]["reason"] == "stuck_drafting_not_actionable");
  }

  SECTION("unknown kind is accepted false") {
    auto req = make_request(http::verb::post,
                            "/ai/nudges/evaluate",
                            R"({
                              "kind":"unknown.kind",
                              "project_id":"proj-1",
                              "created_at":123,
                              "facts":{}
                            })");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res, &service));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["accepted"] == false);
    REQUIRE(payload["data"]["should_nudge"] == false);
    REQUIRE(payload["data"]["reason"] == "unknown_candidate_kind");
  }
}

TEST_CASE("AiNudgeRoutes lists and dismisses created nudges", "[ai][nudges]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1");
  create_card_fixture(db, "proj-1", "card-1");
  holder::ai::NudgeService service(db);

  auto create_req = make_request(http::verb::post,
                                 "/ai/nudges/evaluate",
                                 R"({
                                   "kind":"card.title_only",
                                   "project_id":"proj-1",
                                   "card_id":"card-1",
                                   "created_at":123,
                                   "basis_fingerprint":"abc123",
                                   "facts":{
                                     "title":"Frog",
                                     "body_empty":true,
                                     "doc_chars":12,
                                     "body_chars":0
                                   }
                                 })");
  http::response<http::string_body> create_res;
  REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", create_req, create_res, &service));
  const auto created_payload = nlohmann::json::parse(create_res.body());
  const auto nudge_id = created_payload["data"]["nudge"]["nudge_id"].get<std::string>();

  SECTION("list returns created nudge for matching card") {
    auto req = make_request(http::verb::get, "/ai/nudges?project_id=proj-1&card_id=card-1", "");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges", req, res, &service));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["nudges"].is_array());
    REQUIRE(payload["data"]["nudges"].size() == 1);
    REQUIRE(payload["data"]["nudges"][0]["nudge_id"] == nudge_id);
  }

  SECTION("dismiss hides nudge from subsequent list") {
    auto dismiss_req = make_request(http::verb::post,
                                    "/ai/nudges/" + nudge_id + "/dismiss",
                                    "");
    http::response<http::string_body> dismiss_res;
    REQUIRE(holder::api::routes::handle_ai_nudge_routes(
        "/ai/nudges/" + nudge_id + "/dismiss", dismiss_req, dismiss_res, &service));
    REQUIRE(dismiss_res.result() == http::status::ok);

    auto list_req = make_request(http::verb::get, "/ai/nudges?project_id=proj-1&card_id=card-1", "");
    http::response<http::string_body> list_res;
    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges", list_req, list_res, &service));

    const auto payload = nlohmann::json::parse(list_res.body());
    REQUIRE(payload["data"]["nudges"].is_array());
    REQUIRE(payload["data"]["nudges"].empty());
  }
}
