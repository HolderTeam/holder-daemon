#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/HttpServer.h"
#include "core/Signal.h"
#include "store/Db.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_http_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

nlohmann::json get_health(const std::string& bind,
                          unsigned short port,
                          const std::string& token) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bind, std::to_string(port));

  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{http::verb::get, "/health", 11};
  req.set(http::field::host, bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);

  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);

  socket.shutdown(tcp::socket::shutdown_both);

  REQUIRE(res.result() == http::status::ok);
  return nlohmann::json::parse(res.body());
}

} // namespace

TEST_CASE("HTTP /health returns ok with valid token", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token);
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
