#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiProviderRoutes.h"
#include "http_test_helpers.h"
#include "privacy/SecretStore.h"

#include <boost/beast/http.hpp>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("AiProviderRoutes returns false when no provider sub-route matches", "[ai][providers]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  auto secret_store = holder::privacy::make_default_secret_store(dir);

  auto req = make_request(http::verb::get, "/ai/providers/unknown");
  http::response<http::string_body> res;

  const bool handled =
      holder::api::routes::handle_ai_provider_routes(
          "/ai/providers/unknown", req, res, db, *secret_store);
  REQUIRE_FALSE(handled);
}
