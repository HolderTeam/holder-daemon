#include "http_test_helpers.h"

using holder::test::http_request_raw;
using holder::test::make_temp_dir;

TEST_CASE("HTTP ai_catalog.yaml is served without auth (cloud test alias)", "[http]") {
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

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto res = http_request_raw(bound.bind,
                                    bound.port,
                                    std::string(),
                                    boost::beast::http::verb::get,
                                    "/ai_catalog.yaml");
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("application/yaml") != std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}
