#include "api/Session.h"

#include "api/Router.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <chrono>
#include <string>
#include <thread>

namespace {

using tcp = boost::asio::ip::tcp;

struct SocketPair {
  boost::asio::io_context ioc;
  tcp::socket client;
  tcp::socket server;

  SocketPair() : client(ioc), server(ioc) {
    tcp::acceptor acceptor(ioc, tcp::endpoint(tcp::v4(), 0));
    client.connect(acceptor.local_endpoint());
    server = acceptor.accept();
  }
};

} // namespace

TEST_CASE("Session handles end_of_stream read and returns", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  pair.client.shutdown(tcp::socket::shutdown_send);

  holder::api::Session session(std::move(pair.server),
                               db,
                               token,
                               router,
                               std::chrono::steady_clock::now(),
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr);
  REQUIRE_NOTHROW(session.run());
}

TEST_CASE("Session handles generic read error and returns", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // Send invalid HTTP bytes so beast reports a read error that is not end_of_stream.
  const std::array<char, 12> bad = {'n','o','t','-','h','t','t','p','\r','\n','\r','\n'};
  boost::asio::write(pair.client, boost::asio::buffer(bad));
  pair.client.shutdown(tcp::socket::shutdown_send);

  holder::api::Session session(std::move(pair.server),
                               db,
                               token,
                               router,
                               std::chrono::steady_clock::now(),
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr);
  REQUIRE_NOTHROW(session.run());
}

TEST_CASE("Session handles write error and returns", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // Send a valid request, then force client-side RST so server write fails.
  const std::string req =
      "GET /health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Authorization: Bearer testtoken\r\n"
      "Connection: close\r\n"
      "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));
  pair.client.set_option(boost::asio::socket_base::linger(true, 0));
  pair.client.close();

  holder::api::Session session(std::move(pair.server),
                               db,
                               token,
                               router,
                               std::chrono::steady_clock::now(),
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr);
  REQUIRE_NOTHROW(session.run());
}

TEST_CASE("Session handles normal request/response path", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // No auth header: should produce a normal 401 response and hit final request log path.
  const std::string req =
      "GET /health HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n"
      "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  holder::api::Session session(std::move(pair.server),
                               db,
                               token,
                               router,
                               std::chrono::steady_clock::now(),
                               nullptr,
                               nullptr,
                               nullptr,
                               nullptr);
  REQUIRE_NOTHROW(session.run());

  boost::system::error_code ec;
  std::string response;
  std::array<char, 1024> buf{};
  for (;;) {
    const auto n = pair.client.read_some(boost::asio::buffer(buf), ec);
    if (ec) {
      break;
    }
    response.append(buf.data(), n);
  }
  REQUIRE(ec == boost::asio::error::eof);
  REQUIRE(response.find("401 Unauthorized") != std::string::npos);
}

TEST_CASE("Session prepare_request classifies card patch as save lane", "[session]") {
  SocketPair pair;

  const std::string req =
      "PATCH /cards/card-123 HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Content-Length: 2\r\n"
      "Connection: close\r\n"
      "\r\n"
      "{}";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  auto prepared = holder::api::Session::prepare_request(std::move(pair.server));
  REQUIRE(prepared.has_value());
  REQUIRE(prepared->path == "/cards/card-123");
  REQUIRE(prepared->lane == holder::api::Session::RequestLane::Save);
}

TEST_CASE("Session prepare_request classifies links reads as background lane", "[session]") {
  SocketPair pair;

  const std::string req =
      "GET /cards/card-123/links HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n"
      "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  auto prepared = holder::api::Session::prepare_request(std::move(pair.server));
  REQUIRE(prepared.has_value());
  REQUIRE(prepared->path == "/cards/card-123/links");
  REQUIRE(prepared->lane == holder::api::Session::RequestLane::Background);
}

TEST_CASE("Session prepare_request classifies project reads as foreground lane", "[session]") {
  SocketPair pair;

  const std::string req =
      "GET /projects HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n"
      "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  auto prepared = holder::api::Session::prepare_request(std::move(pair.server));
  REQUIRE(prepared.has_value());
  REQUIRE(prepared->path == "/projects");
  REQUIRE(prepared->lane == holder::api::Session::RequestLane::Foreground);
}
