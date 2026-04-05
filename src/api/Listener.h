#pragma once

#include "ai/NudgeService.h"
#include "api/Router.h"
#include "platform/Signal.h"
#include "git/GitOps.h"
#include "llm/RunnerRegistry.h"
#include "index/FtsIndexer.h"
#include "card/CardStore.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <boost/asio.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace holder::api {

class Listener {
public:
  struct BoundInfo {
    std::string bind;
    unsigned short port = 0;
  };

  Listener(std::string bind,
           unsigned short port,
           holder::platform::Db& db,
           const std::string& auth_token,
           const Router& router,
           std::chrono::steady_clock::time_point started_at,
           holder::card::CardStore* card_store,
           holder::index::FtsIndexer* fts,
           holder::ai::NudgeService* nudge_service,
           holder::privacy::SecretStore* secret_store = nullptr,
           holder::git::GitOps* git_ops = nullptr,
           holder::llm::RunnerRegistry* runner_registry = nullptr);

  BoundInfo start();
  void run(const holder::core::SignalHandler& signals);
  void stop();

private:
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc_;
  tcp::acceptor acceptor_;
  std::string bind_;
  unsigned short port_;
  holder::platform::Db& db_;
  const std::string& auth_token_;
  const Router& router_;
  std::chrono::steady_clock::time_point started_at_;
  holder::card::CardStore* card_store_ = nullptr;
  holder::index::FtsIndexer* fts_ = nullptr;
  holder::ai::NudgeService* nudge_service_ = nullptr;
  holder::privacy::SecretStore* secret_store_ = nullptr;
  holder::git::GitOps* git_ops_ = nullptr;
  holder::llm::RunnerRegistry* runner_registry_ = nullptr;

  std::mutex session_queue_mutex_;
  std::condition_variable session_queue_cv_;
  std::deque<tcp::socket> pending_sessions_;
  std::atomic<bool> stop_requested_{false};
  std::thread session_worker_;

  void run_session_worker();
};

} // namespace holder::api
