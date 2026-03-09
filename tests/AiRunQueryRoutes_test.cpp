#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/runs/AiRunQueryRoutes.h"
#include "http_test_helpers.h"
#include "ai/AiRunRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {

namespace http = boost::beast::http;

} // namespace

TEST_CASE("AiRunQueryRoutes list route validates params and supports project_id", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  http::response<http::string_body> res;
  auto missing = holder::api::routes::ai::runs::handle_ai_runs_list_route(
      [](const std::string&) { return std::string(); }, res, db);
  REQUIRE(missing.handled == true);
  REQUIRE(res.result() == http::status::bad_request);
  const auto missing_payload = nlohmann::json::parse(res.body());
  REQUIRE(missing_payload["error"]["message"] == "Missing project_id or thread_id.");

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-1";
  run.project_id = "proj-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "completed";
  run.created_at = 1;
  run.updated_at = 2;
  repo.create(run);

  auto listed = holder::api::routes::ai::runs::handle_ai_runs_list_route(
      [](const std::string& key) { return key == "project_id" ? std::string("proj-1") : std::string(); },
      res,
      db);
  REQUIRE(listed.handled == true);
  REQUIRE(res.result() == http::status::ok);
  const auto list_payload = nlohmann::json::parse(res.body());
  REQUIRE(list_payload["ok"] == true);
  REQUIRE(list_payload["data"].is_array());
  REQUIRE(list_payload["data"].size() == 1);
  REQUIRE(list_payload["data"][0]["run_id"] == "run-1");
}

TEST_CASE("AiRunQueryRoutes get route handles fallback policy parsing and not_found", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::ai::AiRunRepo repo(db);

  holder::model::AiRun legacy;
  legacy.run_id = "run-legacy";
  legacy.project_id = "proj-1";
  legacy.mode = "auto";
  legacy.prompt = "legacy";
  legacy.ranked_json = std::string("{\"path\":\"cloud\",\"attempts\":[]}");
  legacy.status = "completed";
  legacy.created_at = 1;
  legacy.updated_at = 2;
  repo.create(legacy);

  holder::model::AiRun malformed;
  malformed.run_id = "run-malformed";
  malformed.project_id = "proj-1";
  malformed.mode = "auto";
  malformed.prompt = "bad";
  malformed.policy_trace_json = std::string("{");
  malformed.ranked_json = std::string("[1,2,3]");
  malformed.status = "failed";
  malformed.created_at = 1;
  malformed.updated_at = 2;
  repo.create(malformed);

  http::response<http::string_body> res;
  auto got_legacy =
      holder::api::routes::ai::runs::handle_ai_runs_get_route("/ai/runs/run-legacy", res, db);
  REQUIRE(got_legacy.handled == true);
  REQUIRE(res.result() == http::status::ok);
  const auto legacy_payload = nlohmann::json::parse(res.body());
  REQUIRE(legacy_payload["data"]["policy_trace"].is_object());
  REQUIRE(legacy_payload["data"]["policy_trace"]["path"] == "cloud");

  auto got_malformed =
      holder::api::routes::ai::runs::handle_ai_runs_get_route("/ai/runs/run-malformed", res, db);
  REQUIRE(got_malformed.handled == true);
  REQUIRE(res.result() == http::status::ok);
  const auto malformed_payload = nlohmann::json::parse(res.body());
  REQUIRE(malformed_payload["data"]["policy_trace"].is_null());

  auto not_found =
      holder::api::routes::ai::runs::handle_ai_runs_get_route("/ai/runs/does-not-exist", res, db);
  REQUIRE(not_found.handled == true);
  REQUIRE(res.result() == http::status::not_found);

  auto empty_id = holder::api::routes::ai::runs::handle_ai_runs_get_route("/ai/runs/", res, db);
  REQUIRE(empty_id.handled == true);
  REQUIRE(res.result() == http::status::not_found);
}

TEST_CASE("AiRunQueryRoutes list/get return bad_request when DB access throws", "[http]") {
  holder::platform::Db unopened_db;
  http::response<http::string_body> res;

  auto list_out = holder::api::routes::ai::runs::handle_ai_runs_list_route(
      [](const std::string& key) { return key == "project_id" ? std::string("proj-1") : std::string(); },
      res,
      unopened_db);
  REQUIRE(list_out.handled == true);
  REQUIRE(res.result() == http::status::bad_request);

  auto get_out = holder::api::routes::ai::runs::handle_ai_runs_get_route("/ai/runs/run-1", res, unopened_db);
  REQUIRE(get_out.handled == true);
  REQUIRE(res.result() == http::status::bad_request);
}
