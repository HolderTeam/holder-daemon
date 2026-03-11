#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/threads/AiThreadCollectionRoutes.h"
#include "card/CardRepo.h"
#include "http_test_helpers.h"
#include "model/Card.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {

namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method,
                                               const std::string& target,
                                               const std::string& body = "") {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();
  }
  return req;
}

} // namespace

TEST_CASE("AiThreadCollectionRoutes guards and GET error handling", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto uuid_v4 = []() -> std::string { return "unused"; };
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  SECTION("non matching route returns false") {
    auto req = make_request(http::verb::get, "/ai/threadz");
    http::response<http::string_body> res;
    REQUIRE_FALSE(holder::api::routes::ai::threads::handle_ai_thread_collection_routes(
        "/ai/threadz", req, res, db, uuid_v4, param_get));
  }

  SECTION("GET catches repo exception as bad_request") {
    db.exec("DROP TABLE ai_threads;");
    auto req = make_request(http::verb::get, "/ai/threads");
    http::response<http::string_body> res;
    const auto param_project = [](const std::string& key) -> std::string {
      if (key == "project_id") return "proj-1";
      return {};
    };
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_collection_routes(
        "/ai/threads", req, res, db, uuid_v4, param_project));
    REQUIRE(res.result() == http::status::bad_request);
  }
}

TEST_CASE("AiThreadCollectionRoutes POST branch coverage", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  holder::card::CardRepo card_repo(db);
  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = "proj-1";
  card.title = "Linked";
  card.rel_path = "cards/c1.md";
  card.sort_key = 0.0;
  card.created_at = 5;
  card.updated_at = 5;
  card_repo.create(card);

  const auto param_get = [](const std::string&) -> std::string { return {}; };

  SECTION("POST missing required fields") {
    auto req = make_request(http::verb::post, "/ai/threads", R"({"project_id":"proj-1"})");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_collection_routes(
        "/ai/threads", req, res, db, []() { return "unused"; }, param_get));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Missing required fields.");
  }

  SECTION("POST generates thread_id and defaults timestamps and optional card_id") {
    auto req = make_request(http::verb::post,
                            "/ai/threads",
                            R"({"project_id":"proj-1","title":"T","card_id":"card-1"})");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_collection_routes(
        "/ai/threads", req, res, db, []() { return "generated-id"; }, param_get));
    REQUIRE(res.result() == http::status::created);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["thread_id"] == "generated-id");

    auto get_req = make_request(http::verb::get, "/ai/threads");
    http::response<http::string_body> get_res;
    const auto param_project = [](const std::string& key) -> std::string {
      if (key == "project_id") return "proj-1";
      return {};
    };
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_collection_routes(
        "/ai/threads", get_req, get_res, db, []() { return "unused"; }, param_project));
    REQUIRE(get_res.result() == http::status::ok);
    const auto listed = nlohmann::json::parse(get_res.body());
    REQUIRE(listed["data"].is_array());
    REQUIRE(listed["data"].size() == 1);
    REQUIRE(listed["data"][0]["thread_id"] == "generated-id");
    REQUIRE(listed["data"][0]["card_id"] == "card-1");
    REQUIRE(listed["data"][0]["created_at"].get<long long>() > 0);
    REQUIRE(listed["data"][0]["updated_at"] == listed["data"][0]["created_at"]);
  }

  SECTION("POST maps conflict-prefixed exceptions to conflict response") {
    auto req = make_request(http::verb::post, "/ai/threads", R"({"project_id":"proj-1","title":"A"})");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_collection_routes(
        "/ai/threads",
        req,
        res,
        db,
        []() -> std::string { throw std::runtime_error("conflict: synthetic"); },
        param_get));
    REQUIRE(res.result() == http::status::conflict);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["code"] == "conflict");
  }

  SECTION("POST bad json maps to bad_request catch path") {
    auto req = make_request(http::verb::post, "/ai/threads", "not-json");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_collection_routes(
        "/ai/threads", req, res, db, []() { return "unused"; }, param_get));
    REQUIRE(res.result() == http::status::bad_request);
  }
}
