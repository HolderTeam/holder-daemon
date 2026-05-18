#include "http_test_helpers.h"

using holder::test::get_health;
using holder::test::make_temp_dir;
using holder::test::wait_for_http_health_ready;

TEST_CASE("HTTP /health returns ok with valid token", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
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
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });

  REQUIRE(wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto payload = get_health(bound.bind, bound.port, token);
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["db_ok"] == true);
  REQUIRE(payload["data"]["api_version"] == "0.1");
  REQUIRE(payload["data"]["privacy"].is_object());
  REQUIRE(payload["data"]["privacy"]["backend"] == "libsodium_xchacha20poly1305_ietf");
  REQUIRE(payload["data"]["privacy"]["project_mode_supported"] == true);

  server.stop();
  server_thread.join();
}

TEST_CASE("HTTP /health reports db_ok false when DB is closed", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);
  db.close();

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

  REQUIRE(wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto payload = get_health(bound.bind, bound.port, token);
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["db_ok"] == false);
  REQUIRE(payload["data"]["privacy"].is_object());
  REQUIRE(payload["data"]["privacy"]["backend"] == "libsodium_xchacha20poly1305_ietf");
  REQUIRE(payload["data"]["privacy"]["project_mode_supported"] == true);

  server.stop();
  server_thread.join();
}
