#pragma once

#include "api/Router.h"
#include "store/CardStore.h"
#include "store/Db.h"

#include <boost/asio.hpp>

#include <chrono>
#include <string>

namespace holder::api {

class Session {
public:
  using tcp = boost::asio::ip::tcp;

  Session(tcp::socket socket,
          holder::store::Db& db,
          const std::string& auth_token,
          const Router& router,
          std::chrono::steady_clock::time_point started_at,
          holder::store::CardStore* card_store);

  void run();

private:
  tcp::socket socket_;
  holder::store::Db& db_;
  const std::string& auth_token_;
  const Router& router_;
  std::chrono::steady_clock::time_point started_at_;
  holder::store::CardStore* card_store_ = nullptr;
};

} // namespace holder::api
