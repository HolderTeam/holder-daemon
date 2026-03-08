#pragma once

#include "api/Router.h"
#include "git/GitOps.h"
#include "llm/LocalModelRunner.h"
#include "index/FtsIndexer.h"
#include "card/CardStore.h"
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
          holder::card::CardStore* card_store,
          holder::index::FtsIndexer* fts,
          holder::git::GitOps* git_ops = nullptr,
          holder::llm::LocalModelRunner* runner = nullptr);

  void run();

private:
  tcp::socket socket_;
  holder::store::Db& db_;
  const std::string& auth_token_;
  const Router& router_;
  std::chrono::steady_clock::time_point started_at_;
  holder::card::CardStore* card_store_ = nullptr;
  holder::index::FtsIndexer* fts_ = nullptr;
  holder::git::GitOps* git_ops_ = nullptr;
  holder::llm::LocalModelRunner* runner_ = nullptr;
};

} // namespace holder::api
