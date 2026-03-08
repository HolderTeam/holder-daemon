#include "http_test_helpers.h"

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::create_project;

TEST_CASE("HTTP endpoints reject invalid JSON bodies", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1", (dir / "repo").string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));
  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{http::verb::post, "/cards", 11};
  req.set(http::field::host, bound.bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);
  req.set(http::field::content_type, "application/json");
  req.body() = "{";
  req.prepare_payload();

  http::write(socket, req);
  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);
  socket.shutdown(tcp::socket::shutdown_both);

  REQUIRE(res.result() == http::status::bad_request);

  std::raise(SIGTERM);
  server_thread.join();
}
