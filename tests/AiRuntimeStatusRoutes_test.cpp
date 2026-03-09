#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/status/AiRuntimeStatusRoutes.h"
#include "http_test_helpers.h"
#include "llm/LocalModelRunner.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {

namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("AiRuntimeStatusRoutes handles runner status and retry payloads", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  (void)runner.retry();
  (void)runner.start_pull("fake-echo");

  SECTION("GET /ai/status with runner includes pull details") {
    auto req = make_request(http::verb::get, "/ai/status");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::ai::status::handle_ai_runtime_status_routes(
        "/ai/status", req, res, db, &runner));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["runner_available"] == true);
    REQUIRE(payload["data"]["runner_error"].is_null());
    REQUIRE(payload["data"]["runner_version"] == "fake");
    REQUIRE(payload["data"]["pulls"].is_array());
    REQUIRE(payload["data"]["active_pull_jobs"].is_number_integer());

    if (!payload["data"]["pulls"].empty()) {
      const auto& pull = payload["data"]["pulls"][0];
      REQUIRE(pull["job_id"].is_string());
      REQUIRE(pull["model"].is_string());
      REQUIRE(pull["status"].is_string());
      REQUIRE(pull["updated_at"].is_number_integer());
      REQUIRE(pull["error"].is_null());
      REQUIRE(pull["progress"]["completed"].is_number_integer());
      REQUIRE(pull["progress"]["total"].is_number_integer());
      REQUIRE(pull["progress"]["percent"].is_number());
      REQUIRE(pull["progress"]["stage"].is_string());
      REQUIRE(pull["active"].is_boolean());
    }
  }

  SECTION("POST /ai/runner/retry with runner returns status payload") {
    auto req = make_request(http::verb::post, "/ai/runner/retry");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::ai::status::handle_ai_runtime_status_routes(
        "/ai/runner/retry", req, res, db, &runner));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["runner_available"] == true);
    REQUIRE(payload["data"]["spawn_attempted"] == false);
    REQUIRE(payload["data"]["version"] == "fake");
    REQUIRE(payload["data"]["error"].is_null());
    REQUIRE(payload["data"]["models"].is_array());
    REQUIRE(payload["data"]["models"].size() == 1);
    REQUIRE(payload["data"]["models"][0]["name"] == "fake-echo");
  }
}

TEST_CASE("AiRuntimeStatusRoutes recovers when ai_runs count query cannot be prepared", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("DROP TABLE ai_runs;");

  auto req = make_request(http::verb::get, "/ai/status");
  http::response<http::string_body> res;
  REQUIRE(holder::api::routes::ai::status::handle_ai_runtime_status_routes(
      "/ai/status", req, res, db, nullptr));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["active_runs"] == 0);
}
