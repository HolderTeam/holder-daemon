#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/TrashRoutes.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>

namespace {
namespace http = boost::beast::http;
} // namespace

TEST_CASE("TrashRoutes returns false for unmatched path", "[trash-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  http::request<http::string_body> req{http::verb::get, "/not-trash", 11};
  req.set(http::field::host, "127.0.0.1");
  http::response<http::string_body> res;
  const auto param_get = [](const std::string&) { return std::string(); };

  const bool handled =
      holder::api::routes::handle_trash_routes("/not-trash", req, res, db, nullptr, nullptr, param_get);
  REQUIRE_FALSE(handled);
}
