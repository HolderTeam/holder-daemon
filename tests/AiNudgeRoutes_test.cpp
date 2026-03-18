#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiNudgeRoutes.h"

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

} // namespace

TEST_CASE("AiNudgeRoutes evaluates known nudge candidates", "[ai][nudges]") {
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

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["kind"] == "card.title_only");
    REQUIRE(payload["data"]["accepted"] == true);
    REQUIRE(payload["data"]["should_nudge"] == true);
    REQUIRE(payload["data"]["reason"] == "title_only_candidate_ready");
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

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
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

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["accepted"] == true);
    REQUIRE(payload["data"]["should_nudge"] == true);
    REQUIRE(payload["data"]["reason"] == "git_push_failure_candidate_ready");
  }
}

TEST_CASE("AiNudgeRoutes rejects malformed or non-actionable candidates", "[ai][nudges]") {
  SECTION("unrelated path returns false") {
    auto req = make_request(http::verb::post, "/ai/nudges/nope", "{}");
    http::response<http::string_body> res;

    REQUIRE_FALSE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/nope", req, res));
  }

  SECTION("wrong method returns false") {
    auto req = make_request(http::verb::get, "/ai/nudges/evaluate", "");
    http::response<http::string_body> res;

    REQUIRE_FALSE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
  }

  SECTION("invalid json returns bad request") {
    auto req = make_request(http::verb::post, "/ai/nudges/evaluate", "{");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
    REQUIRE(res.result() == http::status::bad_request);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "invalid_json");
  }

  SECTION("invalid body returns bad request") {
    auto req = make_request(http::verb::post, "/ai/nudges/evaluate", R"({"kind":"card.title_only"})");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
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

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
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

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
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

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
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

    REQUIRE(holder::api::routes::handle_ai_nudge_routes("/ai/nudges/evaluate", req, res));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["accepted"] == false);
    REQUIRE(payload["data"]["should_nudge"] == false);
    REQUIRE(payload["data"]["reason"] == "unknown_candidate_kind");
  }
}
