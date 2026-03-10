#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/status/AiRuntimeStatusRoutes.h"
#include "api/routes/ai/status/AiCapabilitiesRoutes.h"
#include "ai/AiRouterConfigRepo.h"
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

TEST_CASE("AiCapabilitiesRoutes returns router config with project/global effective selection", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::ai::AiRouterConfigRepo repo(db);
  repo.set_global("global-router", 10);
  repo.set_for_project("proj-1", "project-router", 20);

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string& key) -> std::string {
    if (key == "project_id") return "proj-1";
    return {};
  };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, nullptr, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["router_config"]["global"]["router_model"] == "global-router");
  REQUIRE(payload["data"]["router_config"]["project"]["project_id"] == "proj-1");
  REQUIRE(payload["data"]["router_config"]["project"]["router_model"] == "project-router");
  REQUIRE(payload["data"]["router_config"]["effective"]["scope"] == "project");
  REQUIRE(payload["data"]["router_config"]["effective"]["router_model"] == "project-router");
}

TEST_CASE("AiCapabilitiesRoutes falls back to global effective router when project override missing", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::ai::AiRouterConfigRepo repo(db);
  repo.set_global("global-router", 10);

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string& key) -> std::string {
    if (key == "project_id") return "proj-1";
    return {};
  };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, nullptr, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["router_config"]["project"]["router_model"].is_null());
  REQUIRE(payload["data"]["router_config"]["effective"]["scope"] == "global");
  REQUIRE(payload["data"]["router_config"]["effective"]["router_model"] == "global-router");
}

TEST_CASE("AiCapabilitiesRoutes catches router config repo errors", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("DROP TABLE ai_router_config;");

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string& key) -> std::string {
    if (key == "project_id") return "proj-1";
    return {};
  };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, nullptr, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["router_config"]["global"]["router_model"].is_null());
  REQUIRE(payload["data"]["router_config"]["project"]["router_model"].is_null());
  REQUIRE(payload["data"]["router_config"]["effective"]["scope"] == "auto");
}

TEST_CASE("AiCapabilitiesRoutes with runner returns status model list", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.spawn_attempted = true;
  status.last_checked = 123;
  status.version = "runner-v";
  status.error = "runner warning";
  status.models = {
      holder::llm::LocalModel{.name = "m1", .digest = "d1", .size = 11, .modified_at = "t1"},
      holder::llm::LocalModel{.name = "m2", .digest = "d2", .size = 22, .modified_at = "t2"},
  };
  runner.set_status_override_for_tests(status);

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, &runner, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["runner_available"] == true);
  REQUIRE(payload["data"]["spawn_attempted"] == true);
  REQUIRE(payload["data"]["last_checked"] == 123);
  REQUIRE(payload["data"]["version"] == "runner-v");
  REQUIRE(payload["data"]["error"] == "runner warning");
  REQUIRE(payload["data"]["models"].is_array());
  REQUIRE(payload["data"]["models"].size() == 2);
  REQUIRE(payload["data"]["models"][0]["name"] == "m1");
  REQUIRE(payload["data"]["models"][0]["digest"] == "d1");
  REQUIRE(payload["data"]["models"][0]["size"] == 11);
  REQUIRE(payload["data"]["models"][0]["modified_at"] == "t1");
}
