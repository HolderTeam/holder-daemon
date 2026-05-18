#include "http_test_helpers.h"

using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP search endpoints return not_implemented without FTS", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto cards = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/search/cards?project_id=p&q=test",
      nlohmann::json::object(),
      boost::beast::http::status::not_implemented
  );
  REQUIRE(cards["ok"] == false);

  const auto ai = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/search/ai?project_id=p&q=test",
      nlohmann::json::object(),
      boost::beast::http::status::not_implemented
  );
  REQUIRE(ai["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}
