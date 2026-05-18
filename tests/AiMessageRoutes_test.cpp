#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/AiMessageRoutes.h"
#include "http_test_helpers.h"
#include "index/FtsIndexer.h"

#include <boost/beast/http.hpp>
#include <string>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("AiMessageRoutes returns false when no message sub-route matches", "[ai][messages]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::index::FtsIndexer fts(db);

  auto req = make_request(http::verb::get, "/ai/messages/msg-1/other");
  http::response<http::string_body> res;

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  const bool handled = holder::api::routes::handle_ai_message_routes(
      "/ai/messages/msg-1/other",
      req,
      res,
      db,
      &fts,
      uuid_v4,
      param_get
  );
  REQUIRE_FALSE(handled);
}
