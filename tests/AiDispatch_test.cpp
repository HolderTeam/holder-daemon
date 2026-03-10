#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiDispatch.h"
#include "http_test_helpers.h"

#include <boost/asio/ip/tcp.hpp>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("AiDispatch returns unhandled for malformed and unknown routes", "[ai][dispatch]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  SECTION("path without leading slash") {
    auto req = make_request(http::verb::get, "ai/status");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "ai/status", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("root-only slash path") {
    auto req = make_request(http::verb::get, "/");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("status family unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/status/unknown");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/status/unknown", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("providers unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/providers/unknown");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/providers/unknown", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("runs unmatched method/path combo") {
    auto req = make_request(http::verb::delete_, "/ai/runs");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/runs", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("runner unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/runner/unknown");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/runner/unknown", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("threads unmatched subroute") {
    auto req = make_request(http::verb::delete_, "/ai/threads");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/threads", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("messages unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/messages/msg-1/other");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/messages/msg-1/other", req, res, socket, db, nullptr, nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }
}
