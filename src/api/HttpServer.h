#pragma once

#include "api/Router.h"
#include "core/Signal.h"
#include "git/GitOps.h"
#include "index/FtsIndexer.h"
#include "store/Db.h"

#include <chrono>
#include <memory>
#include <string>

namespace holder::store {
class CardStore;
}

namespace holder::api {

class Listener;

class HttpServer {
public:
  struct BoundInfo {
    std::string bind;
    unsigned short port = 0;
  };

  HttpServer(std::string bind,
             unsigned short port,
             holder::store::Db& db,
             std::string auth_token,
             holder::store::CardStore* card_store,
             holder::index::FtsIndexer* fts,
             holder::git::GitOps* git_ops = nullptr);
  ~HttpServer();

  BoundInfo start();
  void run(const holder::core::SignalHandler& signals);

private:
  std::string bind_;
  unsigned short port_;
  holder::store::Db& db_;
  std::string auth_token_;
  std::chrono::steady_clock::time_point started_at_;
  Router router_;
  std::unique_ptr<Listener> listener_;
  holder::store::CardStore* card_store_ = nullptr;
  holder::index::FtsIndexer* fts_ = nullptr;
  holder::git::GitOps* git_ops_ = nullptr;
};

} // namespace holder::api
