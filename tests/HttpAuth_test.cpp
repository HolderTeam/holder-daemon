#include "http_test_helpers.h"

using holder::test::EnvGuard;
using holder::test::http_request_raw;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

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
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto health =
      http_request_raw(bound.bind, bound.port, "", boost::beast::http::verb::get, "/health");
  REQUIRE(health.status == boost::beast::http::status::unauthorized);

  const auto projects =
      http_request_raw(bound.bind, bound.port, "", boost::beast::http::verb::get, "/projects");
  REQUIRE(projects.status == boost::beast::http::status::unauthorized);

  const auto docs =
      http_request_raw(bound.bind, bound.port, "", boost::beast::http::verb::get, "/docs");
  REQUIRE(docs.status == boost::beast::http::status::ok);

  auto raw_with_auth = [&](const std::string& auth_value) {
    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));

    tcp::socket socket(ioc);
    boost::asio::connect(socket, endpoints);

    http::request<http::string_body> req{http::verb::get, "/health", 11};
    req.set(http::field::host, bound.bind);
    req.set(http::field::user_agent, "holder-tests");
    req.set(http::field::authorization, auth_value);

    http::write(socket, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);
    socket.shutdown(tcp::socket::shutdown_both);

    return res.result();
  };

  const auto bad_auth = raw_with_auth("Token nope");
  REQUIRE(bad_auth == boost::beast::http::status::unauthorized);

  const auto missing_bearer = raw_with_auth("testtoken");
  REQUIRE(missing_bearer == boost::beast::http::status::unauthorized);

  const auto lower_bearer = raw_with_auth("bearer " + token);
  REQUIRE(lower_bearer == boost::beast::http::status::unauthorized);

  const auto spaced_bearer = raw_with_auth("Bearer    " + token);
  REQUIRE(spaced_bearer == boost::beast::http::status::unauthorized);

  auto raw_with_multi_auth = [&](const std::string& first, const std::string& second) {
    namespace http = boost::beast::http;
    using tcp = boost::asio::ip::tcp;

    boost::asio::io_context ioc;
    tcp::resolver resolver(ioc);
    auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));

    tcp::socket socket(ioc);
    boost::asio::connect(socket, endpoints);

    http::request<http::string_body> req{http::verb::get, "/health", 11};
    req.set(http::field::host, bound.bind);
    req.set(http::field::user_agent, "holder-tests");
    req.insert(http::field::authorization, first);
    req.insert(http::field::authorization, second);

    http::write(socket, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> res;
    http::read(socket, buffer, res);
    socket.shutdown(tcp::socket::shutdown_both);

    return res.result();
  };

  const auto multi_auth = raw_with_multi_auth("Token nope", "Bearer " + token);
  REQUIRE(multi_auth == boost::beast::http::status::unauthorized);

  std::raise(SIGTERM);
  server_thread.join();
}
