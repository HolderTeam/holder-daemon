#include "http_test_helpers.h"

using holder::test::EnvGuard;
using holder::test::http_request_raw;
using holder::test::make_temp_dir;

TEST_CASE("HTTP docs and openapi are served without auth", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db;
  db.open(db_path);

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

  const auto docs = http_request_raw(bound.bind, bound.port, "",
                                     boost::beast::http::verb::get,
                                     "/docs");
  REQUIRE(docs.status == boost::beast::http::status::ok);
  REQUIRE(docs.content_type.find("text/html") != std::string::npos);

  const auto openapi = http_request_raw(bound.bind, bound.port, "",
                                        boost::beast::http::verb::get,
                                        "/openapi.yaml");
  REQUIRE(openapi.status == boost::beast::http::status::ok);
  REQUIRE(openapi.content_type.find("application/yaml") != std::string::npos);
  REQUIRE(openapi.body.find("openapi:") != std::string::npos);

  server.stop();
  server_thread.join();
}
