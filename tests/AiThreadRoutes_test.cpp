#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiThreadRoutes.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("AiThreadRoutes returns false when no thread sub-route matches", "[ai][threads]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/ai/threadz");
  http::response<http::string_body> res;

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  const bool handled =
      holder::api::routes::handle_ai_thread_routes("/ai/threadz", req, res, db, uuid_v4, param_get);
  REQUIRE_FALSE(handled);
}
