#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/AiResourceRoutes.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method,
                                              const std::string& target,
                                              const nlohmann::json& body = nlohmann::json()) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.is_null() && !body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body.dump();
    req.prepare_payload();
  }
  return req;
}

} // namespace

TEST_CASE("AiResourceRoutes POST maps non-conflict exceptions to bad_request", "[resources][routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  // Force a non-conflict exception inside POST handling: type mismatch on project_id.
  auto req = make_request(http::verb::post,
                          "/resources",
                          nlohmann::json{
                              {"project_id", nlohmann::json::array()},
                              {"kind", "url"},
                              {"uri", "https://example.com"},
                              {"label", "Example"}});
  http::response<http::string_body> res;

  const bool handled =
      holder::api::routes::handle_ai_resource_routes("/resources", req, res, db, uuid_v4, param_get);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("AiResourceRoutes returns false for unrelated paths", "[resources][routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  auto req = make_request(http::verb::get, "/not-a-resource-route");
  http::response<http::string_body> res;

  const bool handled = holder::api::routes::handle_ai_resource_routes(
      "/not-a-resource-route", req, res, db, uuid_v4, param_get);
  REQUIRE_FALSE(handled);
}

