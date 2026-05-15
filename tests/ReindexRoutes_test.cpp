#include "api/routes/ReindexRoutes.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>

TEST_CASE("ReindexRoutes returns false for non-matching route", "[http][reindex]") {
  namespace http = boost::beast::http;
  holder::platform::Db db;
  http::request<http::string_body> req{http::verb::get, "/reindex", 11};
  http::response<http::string_body> res;
  const bool handled = holder::api::routes::handle_reindex_routes("/reindex", req, res, db);
  REQUIRE(!handled);
}

TEST_CASE("ReindexRoutes runs full DB reindex", "[http][reindex]") {
  namespace http = boost::beast::http;
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  http::request<http::string_body> req{http::verb::post, "/reindex", 11};
  http::response<http::string_body> res;
  const bool handled = holder::api::routes::handle_reindex_routes("/reindex", req, res, db);

  REQUIRE(handled);
  REQUIRE(res.result() == http::status::ok);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
}

TEST_CASE("ReindexRoutes reports DB errors", "[http][reindex]") {
  namespace http = boost::beast::http;
  holder::platform::Db db;

  http::request<http::string_body> req{http::verb::post, "/reindex", 11};
  http::response<http::string_body> res;
  const bool handled = holder::api::routes::handle_reindex_routes("/reindex", req, res, db);

  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
}
