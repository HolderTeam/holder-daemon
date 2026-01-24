#pragma once

#include "core/Signal.h"
#include "store/Db.h"

#include <boost/asio.hpp>

#include <chrono>
#include <string>

namespace holder::api {

class HttpServer {
public:
  struct BoundInfo {
    std::string bind;
    unsigned short port = 0;
  };

  HttpServer(std::string bind,
             unsigned short port,
             holder::store::Db& db,
             std::string auth_token);

  BoundInfo start();
  void run(const holder::core::SignalHandler& signals);

private:
  using tcp = boost::asio::ip::tcp;

  void handle_session(tcp::socket& socket);

  boost::asio::io_context ioc_;
  tcp::acceptor acceptor_;
  std::string bind_;
  unsigned short port_;
  holder::store::Db& db_;
  std::string auth_token_;
  std::chrono::steady_clock::time_point started_at_;
};

} // namespace holder::api
