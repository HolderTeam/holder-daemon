#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiRunnerRoutes.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("AiRunnerRoutes returns pull-event dispatch result when handled", "[ai][runner]") {
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);

  auto req = make_request(http::verb::get, "/ai/runner/pull/job-1/events");
  http::response<http::string_body> res;

  const auto out = holder::api::routes::handle_ai_runner_routes(
      "/ai/runner/pull/job-1/events", req, res, socket, nullptr);

  REQUIRE(out.handled);
  REQUIRE_FALSE(out.streamed);
  REQUIRE(res.result() == http::status::not_implemented);
}
