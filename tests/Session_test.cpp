#include "api/Session.h"

#include "api/Router.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <chrono>
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

