#pragma once

#include "ai/NudgeService.h"
#include "api/ConcurrencyProfile.h"
#include "api/Router.h"
#include "api/Session.h"
#include "core/SerialExecutor.h"
#include "platform/Signal.h"
#include "git/ExecutorGitOps.h"
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
#include <vector>

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
           holder::llm::RunnerRegistry* runner_registry = nullptr,
           holder::api::ConcurrencyProfile concurrency = {});

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
  holder::api::ConcurrencyProfile concurrency_;
  holder::git::GitOps* request_git_ops_ = nullptr;
  std::unique_ptr<holder::git::RealGitOps> owned_git_ops_;
  std::unique_ptr<holder::core::SerialExecutor> git_executor_;
  std::unique_ptr<holder::git::ExecutorGitOps> executor_git_ops_;
  std::unique_ptr<holder::core::SerialExecutor> ai_runtime_executor_;

  std::mutex ingress_queue_mutex_;
  std::condition_variable ingress_queue_cv_;
  std::deque<tcp::socket> pending_sockets_;
  std::mutex lane_queue_mutex_;
  std::condition_variable lane_queue_cv_;
  std::deque<Session::PreparedRequest> save_queue_;
  std::deque<Session::PreparedRequest> foreground_queue_;
  std::deque<Session::PreparedRequest> background_queue_;
  std::mutex response_queue_mutex_;
  std::condition_variable response_queue_cv_;
  std::deque<Session::PreparedResponse> response_queue_;
  std::atomic<bool> stop_requested_{false};
  std::vector<std::thread> ingress_workers_;
  std::vector<std::thread> save_workers_;
  std::vector<std::thread> general_workers_;
  std::vector<std::thread> writer_workers_;
  std::vector<std::thread> io_workers_;

  void start_accept_loop();
  void run_ingress_worker();
  void run_save_worker();
  void run_general_worker();
  void run_writer_worker();
};

} // namespace holder::api
