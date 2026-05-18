#include "api/routes/RebuildRoutes.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace http = boost::beast::http;

TEST_CASE("RebuildRoutes returns false for non-matching route", "[http][rebuild]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  http::request<http::string_body> req{http::verb::get, "/rebuild", 11};
  req.body() = "{}";
  req.prepare_payload();
  http::response<http::string_body> res;

  const bool handled =
      holder::api::routes::handle_rebuild_routes("/rebuild", req, res, db, nullptr);
  REQUIRE_FALSE(handled);
}

TEST_CASE("RebuildRoutes returns bad_request when project_id is missing", "[http][rebuild]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  http::request<http::string_body> req{http::verb::post, "/rebuild", 11};
  req.body() = "{}";
  req.prepare_payload();
  http::response<http::string_body> res;

  const bool handled =
      holder::api::routes::handle_rebuild_routes("/rebuild", req, res, db, nullptr);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}
