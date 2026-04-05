#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiRunnerRepo.h"
#include "api/routes/ai/AiRunnerRoutes.h"
#include "http_test_helpers.h"

#include <boost/asio/ip/tcp.hpp>
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

TEST_CASE("AiRunnerRoutes returns pull-event dispatch result when handled", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);

  auto req = make_request(http::verb::get, "/ai/runner/pull/job-1/events");
  http::response<http::string_body> res;

  const auto out = holder::api::routes::handle_ai_runner_routes(
      "/ai/runner/pull/job-1/events",
      req,
      res,
      socket,
      db,
      static_cast<holder::llm::RunnerRegistry*>(nullptr),
      []() { return std::string("generated-id"); },
      [](const std::string&) -> std::string { return {}; });

  REQUIRE(out.handled);
  REQUIRE_FALSE(out.streamed);
  REQUIRE(res.result() == http::status::not_found);
}

TEST_CASE("AiRunnerRoutes supports list create patch and delete", "[ai][runner]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;

  SECTION("GET /ai/runners lists auto-local") {
    auto req = make_request(http::verb::get, "/ai/runners");

    const auto out = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        req,
        res,
        socket,
        db,
        &runner_registry,
        []() { return std::string("ignored"); },
        [](const std::string&) -> std::string { return {}; });

    REQUIRE(out.handled);
    REQUIRE(res.result() == http::status::ok);
    const auto body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["runners"].is_array());
    REQUIRE(body["data"]["runners"].size() == 1);
    REQUIRE(body["data"]["runners"][0]["runner_id"] == "auto-local");
  }

  SECTION("POST then GET then PATCH then DELETE /ai/runners/{id}") {
    auto create = make_request(http::verb::post, "/ai/runners");
    create.set(http::field::content_type, "application/json");
    create.body() = R"({"name":"Office Ollama","kind":"ollama","base_url":"http://office:11434"})";
    create.prepare_payload();

    auto created = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners",
        create,
        res,
        socket,
        db,
        &runner_registry,
        []() { return std::string("runner-123"); },
        [](const std::string&) -> std::string { return {}; });

    REQUIRE(created.handled);
    REQUIRE(res.result() == http::status::created);
    auto body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["runner_id"] == "manual-runner-123");
    REQUIRE(body["data"]["source"] == "manual");
    REQUIRE(body["data"]["runtime"]["configured"] == true);

    auto get_req = make_request(http::verb::get, "/ai/runners/manual-runner-123");
    res = {};
    auto got = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-runner-123",
        get_req,
        res,
        socket,
        db,
        &runner_registry,
        []() { return std::string("ignored"); },
        [](const std::string&) -> std::string { return {}; });

    REQUIRE(got.handled);
    REQUIRE(res.result() == http::status::ok);
    body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["name"] == "Office Ollama");

    auto patch = make_request(http::verb::patch, "/ai/runners/manual-runner-123");
    patch.set(http::field::content_type, "application/json");
    patch.body() = R"({"name":"Desk Ollama","enabled":false})";
    patch.prepare_payload();
    res = {};
    auto patched = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-runner-123",
        patch,
        res,
        socket,
        db,
        &runner_registry,
        []() { return std::string("ignored"); },
        [](const std::string&) -> std::string { return {}; });

    REQUIRE(patched.handled);
    REQUIRE(res.result() == http::status::ok);
    body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["name"] == "Desk Ollama");
    REQUIRE(body["data"]["enabled"] == false);
    REQUIRE(body["data"]["runtime"]["configured"] == false);

    auto del = make_request(http::verb::delete_, "/ai/runners/manual-runner-123");
    res = {};
    auto removed = holder::api::routes::handle_ai_runner_routes(
        "/ai/runners/manual-runner-123",
        del,
        res,
        socket,
        db,
        &runner_registry,
        []() { return std::string("ignored"); },
        [](const std::string&) -> std::string { return {}; });

    REQUIRE(removed.handled);
    REQUIRE(res.result() == http::status::ok);
    body = nlohmann::json::parse(res.body());
    REQUIRE(body["data"]["runner_id"] == "manual-runner-123");
    REQUIRE_FALSE(holder::ai::AiRunnerRepo(db).get("manual-runner-123").has_value());
  }
}
