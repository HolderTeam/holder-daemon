#include "api/Listener.h"

#include "api/Session.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace holder::api {
namespace {

constexpr auto kPollDelay = std::chrono::milliseconds(50);

} // namespace

Listener::Listener(std::string bind,
                   unsigned short port,
                   holder::store::Db& db,
                   const std::string& auth_token,
                   const Router& router,
                   std::chrono::steady_clock::time_point started_at,
                   holder::store::CardStore* card_store)
    : acceptor_(ioc_),
      bind_(std::move(bind)),
      port_(port),
      db_(db),
      auth_token_(auth_token),
      router_(router),
      started_at_(started_at),
      card_store_(card_store) {}

Listener::BoundInfo Listener::start() {
  boost::system::error_code ec;
  const auto address = boost::asio::ip::make_address(bind_, ec);
  if (ec) {
    throw std::runtime_error("Invalid bind address: " + bind_ + " (" + ec.message() + ")");
  }

  tcp::endpoint endpoint(address, port_);
  acceptor_.open(endpoint.protocol(), ec);
  if (ec) throw std::runtime_error("acceptor.open failed: " + ec.message());

  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) throw std::runtime_error("acceptor.set_option failed: " + ec.message());

  acceptor_.bind(endpoint, ec);
  if (ec) throw std::runtime_error("acceptor.bind failed: " + ec.message());

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) throw std::runtime_error("acceptor.listen failed: " + ec.message());

  acceptor_.non_blocking(true, ec);
  if (ec) throw std::runtime_error("acceptor.non_blocking failed: " + ec.message());

  const auto bound = acceptor_.local_endpoint(ec);
  if (ec) throw std::runtime_error("acceptor.local_endpoint failed: " + ec.message());

  return BoundInfo{bound.address().to_string(), bound.port()};
}

void Listener::run(const holder::core::SignalHandler& signals) {
  while (!signals.is_requested()) {
    boost::system::error_code ec;
    tcp::socket socket(ioc_);
    acceptor_.accept(socket, ec);
    if (ec) {
      if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
        std::this_thread::sleep_for(kPollDelay);
        continue;
      }
      spdlog::error("accept failed: {}", ec.message());
      continue;
    }

    Session session(std::move(socket), db_, auth_token_, router_, started_at_, card_store_);
    session.run();
  }
}

} // namespace holder::api
