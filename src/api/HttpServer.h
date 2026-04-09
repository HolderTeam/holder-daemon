#pragma once

#include "ai/NudgeService.h"
#include "api/ConcurrencyProfile.h"
#include "api/Router.h"
#include "platform/Signal.h"
#include "git/GitOps.h"
#include "llm/RunnerRegistry.h"
#include "index/FtsIndexer.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <chrono>
#include <memory>
#include <string>

namespace holder::card {
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
             holder::platform::Db& db,
             std::string auth_token,
             holder::card::CardStore* card_store,
             holder::index::FtsIndexer* fts,
             holder::git::GitOps* git_ops = nullptr,
             holder::llm::RunnerRegistry* runner_registry = nullptr,
             holder::api::ConcurrencyProfile concurrency = {});
  ~HttpServer();

  BoundInfo start();
  void run(const holder::core::SignalHandler& signals);
  void stop();

private:
  std::string bind_;
  unsigned short port_;
  holder::platform::Db& db_;
  std::string auth_token_;
  std::chrono::steady_clock::time_point started_at_;
  Router router_;
  std::unique_ptr<Listener> listener_;
  holder::llm::RunnerRegistry* runner_registry_ = nullptr;
  holder::ai::NudgeService nudge_service_;
  std::unique_ptr<holder::privacy::SecretStore> secret_store_;
  holder::card::CardStore* card_store_ = nullptr;
  holder::index::FtsIndexer* fts_ = nullptr;
  holder::git::GitOps* git_ops_ = nullptr;
  holder::api::ConcurrencyProfile concurrency_;
};

} // namespace holder::api
