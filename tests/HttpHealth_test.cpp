#include "http_test_helpers.h"

using holder::test::get_health;
using holder::test::make_temp_dir;

TEST_CASE("HTTP /health returns ok with valid token", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto payload = get_health(bound.bind, bound.port, token);
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["db_ok"] == true);
  REQUIRE(payload["data"]["api_version"] == "0.1");

  std::raise(SIGTERM);
  server_thread.join();
}
