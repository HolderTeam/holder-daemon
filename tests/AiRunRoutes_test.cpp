#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiRunRoutes.h"
#include "http_test_helpers.h"
#include "index/FtsIndexer.h"
#include "privacy/SecretStore.h"

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

TEST_CASE("AiRunRoutes returns default out when no run sub-route matches", "[ai][runs]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);
  auto secret_store = holder::privacy::make_default_secret_store(dir);

  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  auto req = make_request(http::verb::delete_, "/ai/runs");
  http::response<http::string_body> res;

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  const auto out = holder::api::routes::handle_ai_run_routes(
      "/ai/runs", req, res, socket, db, &fts, secret_store.get(), nullptr, uuid_v4, param_get);
  REQUIRE_FALSE(out.handled);
  REQUIRE_FALSE(out.streamed);
}
