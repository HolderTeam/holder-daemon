#include "http_test_helpers.h"

using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("Listener start fails when port already in use", "[listener]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server1("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server1.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::api::HttpServer server2("127.0.0.1", bound.port, db, token, nullptr, nullptr);
  REQUIRE_THROWS(server2.start());
}

TEST_CASE("HttpServer run throws if start was not called", "[listener]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::core::SignalHandler signals;
  REQUIRE_THROWS(server.run(signals));
}
