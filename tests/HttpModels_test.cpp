#include "http_test_helpers.h"

#include <nlohmann/json.hpp>

using holder::test::http_request_raw;
using holder::test::make_temp_dir;
using holder::test::wait_for_http_health_ready;

TEST_CASE("HTTP ai_catalog.yaml is served without auth", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  const auto ai_catalog_path = std::filesystem::path(SCHEMA_SQL_PATH).parent_path().parent_path() /
                               "config" / "ai_catalog.yaml";
  holder::test::EnvGuard ai_catalog_env("HOLDER_AI_CATALOG_PATH", ai_catalog_path.string());

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

  REQUIRE(wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto res = http_request_raw(bound.bind,
                                    bound.port,
                                    std::string(),
                                    boost::beast::http::verb::get,
                                    "/ai_catalog.yaml");
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("application/yaml") != std::string::npos);

  server.stop();
  server_thread.join();
}

TEST_CASE("HTTP ai_catalog.json is served without auth", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

  const auto ai_catalog_path = std::filesystem::path(SCHEMA_SQL_PATH).parent_path().parent_path() /
                               "config" / "ai_catalog.yaml";
  holder::test::EnvGuard ai_catalog_env("HOLDER_AI_CATALOG_PATH", ai_catalog_path.string());

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

  REQUIRE(wait_for_http_health_ready(bound.bind, bound.port, token));

  const auto res = http_request_raw(bound.bind,
                                    bound.port,
                                    std::string(),
                                    boost::beast::http::verb::get,
                                    "/ai_catalog.json");
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("application/json") != std::string::npos);
  const auto parsed = nlohmann::json::parse(res.body);
  REQUIRE(parsed.contains("models"));
  REQUIRE(parsed["models"].contains("Models"));
  REQUIRE(parsed["models"]["Models"].contains("Cloud"));

  server.stop();
  server_thread.join();
}
