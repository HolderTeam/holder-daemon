#pragma once

#include "api/Router.h"
#include "core/Signal.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "store/CardStore.h"
#include "store/Db.h"

#include <boost/asio.hpp>

#include <chrono>
#include <string>

namespace holder::api {

class Listener {
public:
  struct BoundInfo {
    std::string bind;
    unsigned short port = 0;
  };

  Listener(std::string bind,
           unsigned short port,
           holder::store::Db& db,
           const std::string& auth_token,
           const Router& router,
           std::chrono::steady_clock::time_point started_at,
           holder::store::CardStore* card_store,
           holder::index::FtsIndexer* fts,
           holder::git::GitOps* git_ops = nullptr);

  BoundInfo start();
  void run(const holder::core::SignalHandler& signals);
  void stop();

private:
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc_;
  tcp::acceptor acceptor_;
  std::string bind_;
  unsigned short port_;
  holder::store::Db& db_;
  const std::string& auth_token_;
  const Router& router_;
  std::chrono::steady_clock::time_point started_at_;
  holder::store::CardStore* card_store_ = nullptr;
  holder::index::FtsIndexer* fts_ = nullptr;
  holder::git::GitOps* git_ops_ = nullptr;
};

} // namespace holder::api
