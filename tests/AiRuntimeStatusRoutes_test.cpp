#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/status/AiRuntimeStatusRoutes.h"
#include "api/routes/ai/status/AiCapabilitiesRoutes.h"
#include "ai/AiLocalModelConfigRepo.h"
#include "ai/AiRunnerRepo.h"
#include "http_test_helpers.h"
#include "llm/LocalRunnerClient.h"
#include "llm/LocalModelRunner.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <fstream>

namespace {

namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

std::filesystem::path write_local_meta_catalog(const std::filesystem::path& dir) {
  const auto path = dir / "ai_catalog_capabilities.yaml";
  std::ofstream out(path);
  REQUIRE(out.is_open());
  out << "models:\n";
  out << "  Models:\n";
  out << "    Local:\n";
  out << "      - tag: model-installed\n";
  out << "        hardware_tier: mini\n";
  out << "        quality: high\n";
  out << "        speed: fast\n";
  out << "      - tag: model-to-install\n";
  out << "        hardware_tier: mini\n";
  out << "        quality: medium\n";
  out << "        speed: medium\n";
  return path;
}

} // namespace

TEST_CASE("AiRuntimeStatusRoutes handles runner status and retry payloads", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  (void)runner.retry();
  (void)runner.start_pull("fake-echo");
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);

  SECTION("GET /ai/status with runner includes pull details") {
    auto req = make_request(http::verb::get, "/ai/status");
    http::response<http::string_body> res;

    REQUIRE(holder::api::routes::ai::status::handle_ai_runtime_status_routes(
        "/ai/status",
        req,
        res,
        db,
        &runner_registry,
        [](const std::string&) -> std::string { return {}; }));
    REQUIRE(res.result() == http::status::ok);

    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["runners"].is_array());
    REQUIRE(payload["data"]["runners"].size() == 1);
    REQUIRE(payload["data"]["runners"][0]["runner_id"] == "auto-local");
    REQUIRE(payload["data"]["runners"][0]["runtime"]["models"].is_array());
    REQUIRE(payload["data"]["active_pull_jobs"].is_number_integer());

    const auto& runtime_pulls = payload["data"]["runners"][0]["runtime"]["pulls"];
    REQUIRE(runtime_pulls.is_array());
    if (!runtime_pulls.empty()) {
      const auto& pull = runtime_pulls[0];
      REQUIRE(pull["job_id"].is_string());
      REQUIRE(pull["runner_id"] == "auto-local");
      REQUIRE(pull["model"].is_string());
      REQUIRE(pull["model_ref"] == std::string("auto-local::") + pull["model"].get<std::string>());
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

}

TEST_CASE("AiRuntimeStatusRoutes recovers when ai_runs count query cannot be prepared", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("DROP TABLE ai_runs;");

  auto req = make_request(http::verb::get, "/ai/status");
  http::response<http::string_body> res;
  REQUIRE(holder::api::routes::ai::status::handle_ai_runtime_status_routes(
      "/ai/status",
      req,
      res,
      db,
      nullptr,
      [](const std::string&) -> std::string { return {}; }));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["active_runs"] == 0);
}

TEST_CASE("AiCapabilitiesRoutes returns local model config", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::ai::AiLocalModelConfigRepo repo(db);
  repo.set(std::string("fast-model"), std::string("strong-model"), std::string("deep-model"), 20);

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, nullptr, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["local_model_config"]["fast_model"] == "auto-local::fast-model");
  REQUIRE(payload["data"]["local_model_config"]["strong_model"] == "auto-local::strong-model");
  REQUIRE(payload["data"]["local_model_config"]["deep_model"] == "auto-local::deep-model");
  REQUIRE(payload["data"]["local_model_config"]["updated_at"] == 20);
}

TEST_CASE("AiCapabilitiesRoutes returns null local model config when unset", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, nullptr, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["local_model_config"]["fast_model"].is_null());
  REQUIRE(payload["data"]["local_model_config"]["strong_model"].is_null());
  REQUIRE(payload["data"]["local_model_config"]["deep_model"].is_null());
}

TEST_CASE("AiCapabilitiesRoutes catches local model config repo errors", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("DROP TABLE ai_local_model_config;");

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, nullptr, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["local_model_config"]["fast_model"].is_null());
  REQUIRE(payload["data"]["local_model_config"]["strong_model"].is_null());
  REQUIRE(payload["data"]["local_model_config"]["deep_model"].is_null());
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
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, &runner_registry, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["runners"].is_array());
  REQUIRE(payload["data"]["runners"].size() == 1);
  REQUIRE(payload["data"]["runners"][0]["runner_id"] == "auto-local");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["available"] == true);
  REQUIRE(payload["data"]["runners"][0]["runtime"]["spawn_attempted"] == true);
  REQUIRE(payload["data"]["runners"][0]["runtime"]["last_checked"] == 123);
  REQUIRE(payload["data"]["runners"][0]["runtime"]["version"] == "runner-v");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["error"] == "runner warning");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["models"][0]["model_ref"] == "auto-local::m1");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["models"][0]["runner_id"] == "auto-local");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["models"][0]["name"] == "m1");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["models"][0]["model_ref"] == "auto-local::m1");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["models"][0]["digest"] == "d1");
  REQUIRE(payload["data"]["runners"][0]["runtime"]["models"][0]["size"] == 11);
  REQUIRE(payload["data"]["runners"][0]["runtime"]["models"][0]["modified_at"] == "t1");
}

TEST_CASE("AiCapabilitiesRoutes without runner includes recommendation entries", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto catalog = write_local_meta_catalog(dir);
  holder::test::EnvGuard catalog_env("HOLDER_AI_CATALOG_PATH", catalog.string());

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, nullptr, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["recommended_models"].is_array());
  REQUIRE(payload["data"]["recommended_models"].size() == 2);
  REQUIRE(payload["data"]["recommended_install"].is_array());
  REQUIRE(payload["data"]["recommended_install"].size() == 2);
}

TEST_CASE("AiCapabilitiesRoutes with runner separates installed recommendations", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto catalog = write_local_meta_catalog(dir);
  holder::test::EnvGuard catalog_env("HOLDER_AI_CATALOG_PATH", catalog.string());

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.spawn_attempted = true;
  status.last_checked = 456;
  status.version = "runner-v";
  status.models = {
      holder::llm::LocalModel{.name = "model-installed", .digest = "d", .size = 11, .modified_at = "t"},
  };
  runner.set_status_override_for_tests(status);
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);

  auto req = make_request(http::verb::get, "/ai/capabilities");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) -> std::string { return {}; };

  REQUIRE(holder::api::routes::ai::status::handle_ai_capabilities_routes(
      "/ai/capabilities", req, res, db, &runner_registry, param_get));
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["recommended_models"].is_array());
  REQUIRE(payload["data"]["recommended_models"].size() == 2);
  REQUIRE(payload["data"]["recommended_install"].is_array());
  REQUIRE(payload["data"]["recommended_install"].size() == 1);
  REQUIRE(payload["data"]["runners"].is_array());
  REQUIRE(payload["data"]["runners"].size() == 1);
  REQUIRE(payload["data"]["recommended_install"][0]["tag"] == "model-to-install");
  REQUIRE(payload["data"]["recommended_install"][0]["installed"] == false);
}
