#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ProjectRoutes.h"
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

TEST_CASE("ProjectRoutes returns false when path does not match", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/not-projects");
  http::response<http::string_body> res;
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  const bool handled = holder::api::routes::handle_project_routes(
      "/not-projects", req, res, db, nullptr, uuid_v4, param_get);
  REQUIRE_FALSE(handled);
}

TEST_CASE("ProjectRoutes global recovery import validates required fields", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::post, "/recovery-token/import", nlohmann::json::object());
  http::response<http::string_body> res;
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  const bool handled = holder::api::routes::handle_project_routes(
      "/recovery-token/import", req, res, db, nullptr, uuid_v4, param_get);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("ProjectRoutes rejects empty project id segment", "[project-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/projects/");
  http::response<http::string_body> res;
  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  const bool handled = holder::api::routes::handle_project_routes(
      "/projects/", req, res, db, nullptr, uuid_v4, param_get);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::not_found);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "not_found");
}

