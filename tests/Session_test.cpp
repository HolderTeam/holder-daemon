#include "api/Session.h"

#include "api/Router.h"
#include "platform/Db.h"

#include <catch2/catch_test_macros.hpp>

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <mutex>
#include <string>
#include <thread>

namespace {

using tcp = boost::asio::ip::tcp;

struct SocketPair {
  boost::asio::io_context ioc;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard;
  tcp::socket client;
  tcp::socket server;
  std::thread io_thread;

  SocketPair()
      : work_guard(boost::asio::make_work_guard(ioc)),
        client(ioc),
        server(ioc) {
    tcp::acceptor acceptor(ioc, tcp::endpoint(boost::asio::ip::address_v4::loopback(), 0));
    client.connect(acceptor.local_endpoint());
    server = acceptor.accept();
    io_thread = std::thread([this]() {
      ioc.run();
    });
  }

  ~SocketPair() {
    boost::system::error_code ec;
    client.close(ec);
    ec.clear();
    server.close(ec);
    work_guard.reset();
    ioc.stop();
    if (io_thread.joinable()) {
      io_thread.join();
    }
  }
};

} // namespace

TEST_CASE("Session handles end_of_stream read and returns", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  pair.client.shutdown(tcp::socket::shutdown_send);

  holder::api::Session session(
      std::move(pair.server),
      db,
      token,
      router,
      std::chrono::steady_clock::now(),
      nullptr,
      nullptr,
      nullptr,
      nullptr
  );
  REQUIRE_NOTHROW(session.run());
}

TEST_CASE("Session handles generic read error and returns", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // Send invalid HTTP bytes so beast reports a read error that is not end_of_stream.
  const std::array<char, 12> bad = {'n', 'o', 't', '-', 'h', 't', 't', 'p', '\r', '\n', '\r', '\n'};
  boost::asio::write(pair.client, boost::asio::buffer(bad));
  pair.client.shutdown(tcp::socket::shutdown_send);

  holder::api::Session session(
      std::move(pair.server),
      db,
      token,
      router,
      std::chrono::steady_clock::now(),
      nullptr,
      nullptr,
      nullptr,
      nullptr
  );
  REQUIRE_NOTHROW(session.run());
}

TEST_CASE("Session handles write error and returns", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // Send a valid request, then force client-side RST so server write fails.
  const std::string req = "GET /health HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Authorization: Bearer testtoken\r\n"
                          "Connection: close\r\n"
                          "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));
  pair.client.set_option(boost::asio::socket_base::linger(true, 0));
  pair.client.close();

  holder::api::Session session(
      std::move(pair.server),
      db,
      token,
      router,
      std::chrono::steady_clock::now(),
      nullptr,
      nullptr,
      nullptr,
      nullptr
  );
  REQUIRE_NOTHROW(session.run());
}

TEST_CASE("Session handles normal request/response path", "[session]") {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // No auth header: should produce a normal 401 response and hit final request log path.
  const std::string req = "GET /health HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  holder::api::Session session(
      std::move(pair.server),
      db,
      token,
      router,
      std::chrono::steady_clock::now(),
      nullptr,
      nullptr,
      nullptr,
      nullptr
  );
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

TEST_CASE(
    "Session bypasses bearer auth for the Google Drive OAuth callback route",
    "[session]"
) {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // No Authorization header at all -- this is the browser hitting Google's redirect
  // directly, which never carries the daemon's own token (see Session.cpp's own comment
  // on this exact route). No pending OAuth attempt exists for "no-such-location", so this
  // should reach GoogleDriveOAuthRoutes' own "Connection expired" response, not a 401 --
  // proving the bypass is wired, without needing real Google credentials or a real
  // browser round trip.
  const std::string req =
      "GET /locations/no-such-location/oauth/google-drive/callback?state=x&code=y "
      "HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Connection: close\r\n"
      "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  holder::api::Session session(
      std::move(pair.server),
      db,
      token,
      router,
      std::chrono::steady_clock::now(),
      nullptr,
      nullptr,
      nullptr,
      nullptr
  );
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
  REQUIRE(response.find("401 Unauthorized") == std::string::npos);
  REQUIRE(response.find("Connection expired") != std::string::npos);
}

TEST_CASE(
    "Session still requires bearer auth for other /locations routes",
    "[session]"
) {
  SocketPair pair;
  holder::platform::Db db;
  holder::api::Router router;
  const std::string token = "testtoken";

  // A path that merely starts with /locations/ but isn't the callback's exact
  // .../oauth/google-drive/callback suffix must still hit the normal auth gate --
  // guards against the bypass in Session.cpp being accidentally broadened.
  const std::string req = "GET /locations/some-id/binding HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  holder::api::Session session(
      std::move(pair.server),
      db,
      token,
      router,
      std::chrono::steady_clock::now(),
      nullptr,
      nullptr,
      nullptr,
      nullptr
  );
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

TEST_CASE("Session write_prepared_response cancel hook terminates in-flight write", "[session]") {
  namespace http = boost::beast::http;

  SocketPair pair;
  pair.client.set_option(boost::asio::socket_base::receive_buffer_size(1024));

  holder::api::Session::Request req;
  req.method(http::verb::get);
  req.target("/health");
  req.version(11);

  holder::api::Session::Response res;
  res.result(http::status::ok);
  res.version(11);
  res.set(http::field::content_type, "text/plain");
  res.keep_alive(false);
  res.body() = std::string(std::size_t{8} * 1024 * 1024, 'x');
  res.prepare_payload();

  holder::api::Session::PreparedResponse prepared{
      std::move(pair.server),
      std::move(req),
      std::move(res),
      std::chrono::steady_clock::now(),
      holder::api::Session::RequestLane::Foreground,
  };

  std::mutex mutex;
  std::condition_variable cv;
  holder::api::Session::IoHandlePtr active;
  bool done = false;

  auto future = std::async(std::launch::async, [&]() {
    holder::api::Session::write_prepared_response(
        std::move(prepared),
        [&](const holder::api::Session::IoHandlePtr& io_handle) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            active = io_handle;
          }
          cv.notify_one();
        },
        [&](const holder::api::Session::IoHandlePtr&) {
          {
            std::lock_guard<std::mutex> lock(mutex);
            done = true;
          }
          cv.notify_one();
        }
    );
  });

  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&]() {
      return static_cast<bool>(active);
    }));
  }

  active->cancel();

  REQUIRE(future.wait_for(std::chrono::seconds(1)) == std::future_status::ready);

  {
    std::unique_lock<std::mutex> lock(mutex);
    REQUIRE(cv.wait_for(lock, std::chrono::seconds(1), [&]() {
      return done;
    }));
  }
}

TEST_CASE("Session prepare_request classifies card patch as save lane", "[session]") {
  SocketPair pair;

  const std::string req = "PATCH /cards/card-123 HTTP/1.1\r\n"
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

  const std::string req = "GET /cards/card-123/links HTTP/1.1\r\n"
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

  const std::string req = "GET /projects HTTP/1.1\r\n"
                          "Host: localhost\r\n"
                          "Connection: close\r\n"
                          "\r\n";
  boost::asio::write(pair.client, boost::asio::buffer(req));

  auto prepared = holder::api::Session::prepare_request(std::move(pair.server));
  REQUIRE(prepared.has_value());
  REQUIRE(prepared->path == "/projects");
  REQUIRE(prepared->lane == holder::api::Session::RequestLane::Foreground);
}
