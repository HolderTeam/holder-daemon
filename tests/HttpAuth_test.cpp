#include "http_test_helpers.h"

using holder::test::EnvGuard;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::http_request_raw;

TEST_CASE("HTTP endpoints require auth token", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);

  namespace fs = std::filesystem;
  const auto docs_root = dir / "assets" / "swagger-ui";
  fs::create_directories(docs_root);
  std::ofstream(docs_root / "index.html") << "<!doctype html><title>Docs</title>";
  const auto openapi_path = dir / "openapi.yaml";
  std::ofstream(openapi_path) << "openapi: 3.0.0\ninfo:\n  title: Test\n  version: 0.1\n";

  EnvGuard docs_env("HOLDER_DOCS_ROOT", docs_root.string());
  EnvGuard openapi_env("HOLDER_OPENAPI_PATH", openapi_path.string());

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

  const auto health = http_request_raw(bound.bind,
                                       bound.port,
                                       "",
                                       boost::beast::http::verb::get,
                                       "/health");
  REQUIRE(health.status == boost::beast::http::status::unauthorized);

  const auto projects = http_request_raw(bound.bind,
                                         bound.port,
                                         "",
                                         boost::beast::http::verb::get,
                                         "/projects");
  REQUIRE(projects.status == boost::beast::http::status::unauthorized);

  const auto docs = http_request_raw(bound.bind,
                                     bound.port,
                                     "",
                                     boost::beast::http::verb::get,
                                     "/docs");
  REQUIRE(docs.status == boost::beast::http::status::ok);

  const auto bad_auth = http_request_raw(bound.bind,
                                         bound.port,
                                         "Token nope",
                                         boost::beast::http::verb::get,
                                         "/health");
  REQUIRE(bad_auth.status == boost::beast::http::status::unauthorized);

  std::raise(SIGTERM);
  server_thread.join();
}
