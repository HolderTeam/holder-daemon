#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/SearchRoutes.h"
#include "http_test_helpers.h"
#include "index/FtsIndexer.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

std::function<std::string(const std::string&)> map_param_getter(
    const std::unordered_map<std::string, std::string>& params
) {
  return [params](const std::string& key) {
    const auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  };
}

} // namespace

TEST_CASE("SearchRoutes catches search_cards exceptions as bad_request", "[search-routes]") {
  const auto dir = holder::test::make_temp_dir();
  holder::platform::Db db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  db.close(); // force fts->search_cards() to throw deterministically

  auto req = make_request(http::verb::get, "/search/cards");
  http::response<http::string_body> res;
  const auto param_get = map_param_getter({
      {"project_id", "proj-1"},
      {"q", "needle"},
      {"limit", "10"},
      {"offset", "0"},
  });

  const bool handled =
      holder::api::routes::handle_search_routes("/search/cards", req, res, &fts, param_get);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("SearchRoutes catches search_messages exceptions as bad_request", "[search-routes]") {
  const auto dir = holder::test::make_temp_dir();
  holder::platform::Db db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  db.close(); // force fts->search_messages() to throw deterministically

  auto req = make_request(http::verb::get, "/search/ai");
  http::response<http::string_body> res;
  const auto param_get = map_param_getter({
      {"project_id", "proj-1"},
      {"q", "needle"},
      {"limit", "10"},
      {"offset", "0"},
  });

  const bool handled =
      holder::api::routes::handle_search_routes("/search/ai", req, res, &fts, param_get);
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("SearchRoutes returns false for unmatched paths", "[search-routes]") {
  const auto dir = holder::test::make_temp_dir();
  holder::platform::Db db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/search/unknown");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  const bool handled =
      holder::api::routes::handle_search_routes("/search/unknown", req, res, nullptr, param_get);
  REQUIRE_FALSE(handled);
}
