#include "http_test_helpers.h"
#include "api/routes/ai/runner/AiRunnerPullRoutes.h"
#include "llm/LocalRunnerClient.h"
#include "llm/LocalModelRunner.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {

namespace http = boost::beast::http;

holder::api::routes::RunnerRouteDispatchResult call_pull_route(
    const std::string& path,
    http::verb method,
    const std::string& body,
    holder::llm::RunnerRegistry* runner_registry,
    http::response<http::string_body>& res) {
  http::request<http::string_body> req{method, path, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body;
  }
  return holder::api::routes::ai::runner::handle_ai_runner_pull_routes(path, req, res, runner_registry);
}

} // namespace

TEST_CASE("Ai runner pull routes ignore unmatched paths", "[http]") {
  http::response<http::string_body> res;
  auto out = call_pull_route(
      "/ai/runner/unknown", http::verb::get, "", static_cast<holder::llm::RunnerRegistry*>(nullptr), res);
  REQUIRE_FALSE(out.handled);
  REQUIRE_FALSE(out.streamed);
}

TEST_CASE("Ai runner pull routes POST returns unavailable when runner not ready", "[http]") {
  holder::llm::LocalModelRunner runner;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);
  http::response<http::string_body> res;

  auto out =
      call_pull_route("/ai/runner/pull", http::verb::post, R"({"model":"fake-echo"})", &runner_registry, res);
  REQUIRE(out.handled);
  REQUIRE_FALSE(out.streamed);
  REQUIRE(res.result() == http::status::service_unavailable);
  REQUIRE(res.body().find("runner_unavailable") != std::string::npos);
}

TEST_CASE("Ai runner pull routes POST validates request body", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  (void)runner.retry();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);

  SECTION("missing model") {
    http::response<http::string_body> res;
    auto out = call_pull_route("/ai/runner/pull", http::verb::post, R"({"x":"y"})", &runner_registry, res);
    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(res.body().find("Missing model.") != std::string::npos);
  }

  SECTION("invalid JSON catches exception") {
    http::response<http::string_body> res;
    auto out = call_pull_route("/ai/runner/pull", http::verb::post, "{", &runner_registry, res);
    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::bad_request);
    REQUIRE(res.body().find("bad_request") != std::string::npos);
  }
}

TEST_CASE("Ai runner pull routes POST returns error when pull start fails", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  (void)runner.retry();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);
  http::response<http::string_body> res;

  auto out = call_pull_route("/ai/runner/pull", http::verb::post, R"({"model":""})", &runner_registry, res);
  REQUIRE(out.handled);
  REQUIRE(res.result() == http::status::bad_request);
  REQUIRE(res.body().find("missing model") != std::string::npos);
}

TEST_CASE("Ai runner pull routes POST returns job on success", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  (void)runner.retry();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);
  http::response<http::string_body> res;

  auto out =
      call_pull_route("/ai/runner/pull", http::verb::post, R"({"model":"fake-echo"})", &runner_registry, res);
  REQUIRE(out.handled);
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["runner_id"] == "auto-local");
  REQUIRE(payload["data"]["model"] == "fake-echo");
  REQUIRE(payload["data"]["model_ref"] == "auto-local::fake-echo");
  REQUIRE(payload["data"]["status"].is_string());
  REQUIRE(payload["data"]["job_id"].is_string());
  REQUIRE(payload["data"]["job_id"].get<std::string>().rfind("pull-", 0) == 0);
}

TEST_CASE("Ai runner pull routes GET validates job lookup", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);

  SECTION("empty job id") {
    http::response<http::string_body> res;
    auto out = call_pull_route("/ai/runner/pull/", http::verb::get, "", &runner_registry, res);
    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(res.body().find("Pull job not found.") != std::string::npos);
  }

  SECTION("missing job id") {
    http::response<http::string_body> res;
    auto out = call_pull_route("/ai/runner/pull/missing-job", http::verb::get, "", &runner_registry, res);
    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::not_found);
    REQUIRE(res.body().find("Pull job not found.") != std::string::npos);
  }
}

TEST_CASE("Ai runner pull routes GET returns job payload", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  const auto started = runner.start_pull("fake-echo");
  REQUIRE(!started.job_id.empty());
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);

  http::response<http::string_body> res;
  auto out = call_pull_route(
      "/ai/runner/pull/" + started.job_id, http::verb::get, "", &runner_registry, res);
  REQUIRE(out.handled);
  REQUIRE(res.result() == http::status::ok);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["job_id"] == started.job_id);
  REQUIRE(payload["data"]["runner_id"] == "auto-local");
  REQUIRE(payload["data"]["model"] == "fake-echo");
  REQUIRE(payload["data"]["model_ref"] == "auto-local::fake-echo");
  REQUIRE(payload["data"]["status"].is_string());
  REQUIRE(payload["data"]["updated_at"].is_number_integer());
  REQUIRE(payload["data"]["progress"]["completed"].is_number_integer());
  REQUIRE(payload["data"]["progress"]["total"].is_number_integer());
  REQUIRE(payload["data"]["progress"]["percent"].is_number());
  REQUIRE(payload["data"]["progress"]["stage"].is_string());
}
