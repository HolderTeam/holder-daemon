#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/Router.h"

TEST_CASE("Router dispatches to registered handler", "[router]") {
  holder::api::Router router;
  router.add(
      boost::beast::http::verb::get,
      "/ping",
      [](const holder::api::Router::Request&, holder::api::Router::Response& res) {
        res.result(boost::beast::http::status::ok);
        res.body() = "pong";
        res.prepare_payload();
      }
  );

  holder::api::Router::Request req{boost::beast::http::verb::get, "/ping", 11};
  holder::api::Router::Response res;
  const bool handled = router.dispatch(req, res);

  REQUIRE(handled);
  REQUIRE(res.result() == boost::beast::http::status::ok);
  REQUIRE(res.body() == "pong");
}

TEST_CASE("Router returns false for missing routes", "[router]") {
  holder::api::Router router;
  holder::api::Router::Request req{boost::beast::http::verb::get, "/missing", 11};
  holder::api::Router::Response res;
  const bool handled = router.dispatch(req, res);
  REQUIRE_FALSE(handled);
}
