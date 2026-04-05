#include "http_test_helpers.h"
#include "api/routes/ai/runner/AiRunnerPullEventRoutes.h"
#include "llm/LocalRunnerClient.h"
#include "llm/LocalModelRunner.h"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <atomic>
#include <string>
#include <thread>

namespace {

namespace http = boost::beast::http;
using tcp = boost::asio::ip::tcp;

struct StreamResult {
  holder::api::routes::RunnerRouteDispatchResult route_result;
  std::string raw_response;
};

StreamResult invoke_pull_event_route_and_capture(const std::string& path,
                                                 holder::llm::RunnerRegistry* runner_registry) {
  boost::asio::io_context ioc;
  tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});

  const auto endpoint = acceptor.local_endpoint();
  tcp::socket client(ioc);
  client.connect(endpoint);

  tcp::socket server(ioc);
  acceptor.accept(server);

  http::request<http::string_body> req{http::verb::get, path, 11};
  req.set(http::field::host, "127.0.0.1");
  http::response<http::string_body> res;
  holder::api::routes::RunnerRouteDispatchResult route_result{};

  std::thread route_thread([&]() {
    const auto param_get = [](const std::string&) -> std::string { return {}; };
    route_result = holder::api::routes::ai::runner::handle_ai_runner_pull_event_routes(
        path, req, res, server, runner_registry, param_get);
    boost::system::error_code ec;
    server.shutdown(tcp::socket::shutdown_both, ec);
    server.close(ec);
  });

  std::string raw;
  std::array<char, 4096> chunk{};
  for (;;) {
    boost::system::error_code ec;
    const std::size_t n = client.read_some(boost::asio::buffer(chunk), ec);
    if (n > 0) {
      raw.append(chunk.data(), n);
    }
    if (ec == boost::asio::error::eof) {
      break;
    }
    REQUIRE(!ec);
  }

  route_thread.join();
  return {route_result, raw};
}

} // namespace

TEST_CASE("Ai runner pull events route ignores non-GET request", "[http]") {
  boost::asio::io_context ioc;
  tcp::socket socket(ioc);
  http::request<http::string_body> req{http::verb::post, "/ai/runner/pull/job-1/events", 11};
  http::response<http::string_body> res;

  auto out = holder::api::routes::ai::runner::handle_ai_runner_pull_event_routes(
      "/ai/runner/pull/job-1/events",
      req,
      res,
      socket,
      static_cast<holder::llm::RunnerRegistry*>(nullptr),
      [](const std::string&) -> std::string { return {}; });
  REQUIRE_FALSE(out.handled);
  REQUIRE_FALSE(out.streamed);
}

TEST_CASE("Ai runner pull events route returns not_implemented when runner missing", "[http]") {
  boost::asio::io_context ioc;
  tcp::socket socket(ioc);
  http::request<http::string_body> req{http::verb::get, "/ai/runner/pull/job-1/events", 11};
  req.set(http::field::host, "127.0.0.1");
  http::response<http::string_body> res;

  auto out = holder::api::routes::ai::runner::handle_ai_runner_pull_event_routes(
      "/ai/runner/pull/job-1/events",
      req,
      res,
      socket,
      static_cast<holder::llm::RunnerRegistry*>(nullptr),
      [](const std::string&) -> std::string { return {}; });
  REQUIRE(out.handled);
  REQUIRE_FALSE(out.streamed);
  REQUIRE(res.result() == http::status::not_found);
  REQUIRE(res.body().find("not_found") != std::string::npos);
}

TEST_CASE("Ai runner pull events route rejects empty job id path at guard", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);

  boost::asio::io_context ioc;
  tcp::socket socket(ioc);
  http::request<http::string_body> req{http::verb::get, "/ai/runner/pull//events", 11};
  req.set(http::field::host, "127.0.0.1");
  http::response<http::string_body> res;

  auto out = holder::api::routes::ai::runner::handle_ai_runner_pull_event_routes(
      "/ai/runner/pull//events",
      req,
      res,
      socket,
      &runner_registry,
      [](const std::string&) -> std::string { return {}; });
  REQUIRE_FALSE(out.handled);
  REQUIRE_FALSE(out.streamed);
}

TEST_CASE("Ai runner pull events route streams failed event for missing job", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);

  const auto stream =
      invoke_pull_event_route_and_capture("/ai/runner/pull/missing-job/events", &runner_registry);
  REQUIRE(stream.route_result.handled);
  REQUIRE(stream.route_result.streamed);
  REQUIRE(stream.raw_response.find("text/event-stream") != std::string::npos);
  REQUIRE(stream.raw_response.find("event: failed") != std::string::npos);
  REQUIRE(stream.raw_response.find("\"runner_id\":\"auto-local\"") != std::string::npos);
  REQUIRE(stream.raw_response.find("Pull job not found.") != std::string::npos);
}

TEST_CASE("Ai runner pull events route streams progress and completed for job", "[http]") {
  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(true);
  const auto job = runner.start_pull("fake-echo");
  REQUIRE(!job.job_id.empty());
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(nullptr, &local_runner_client);

  const auto stream =
      invoke_pull_event_route_and_capture("/ai/runner/pull/" + job.job_id + "/events", &runner_registry);
  REQUIRE(stream.route_result.handled);
  REQUIRE(stream.route_result.streamed);
  REQUIRE(stream.raw_response.find("text/event-stream") != std::string::npos);
  REQUIRE(stream.raw_response.find("event: progress") != std::string::npos);
  REQUIRE(stream.raw_response.find("event: completed") != std::string::npos);
  REQUIRE(stream.raw_response.find("\"job_id\":\"" + job.job_id + "\"") != std::string::npos);
  REQUIRE(stream.raw_response.find("\"runner_id\":\"auto-local\"") != std::string::npos);
  REQUIRE(stream.raw_response.find("\"model_ref\":\"auto-local::fake-echo\"") != std::string::npos);
}
