#include "api/Listener.h"

#include "api/Session.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace holder::api {
namespace {

constexpr auto kPollDelay = std::chrono::milliseconds(50);
constexpr std::size_t kInitialSessionWorkerCount = 1;
constexpr std::size_t kMaxPendingSessions = 64;

} // namespace

Listener::Listener(std::string bind,
                   unsigned short port,
                   holder::platform::Db& db,
                   const std::string& auth_token,
                   const Router& router,
                   std::chrono::steady_clock::time_point started_at,
                   holder::card::CardStore* card_store,
                   holder::index::FtsIndexer* fts,
                   holder::ai::NudgeService* nudge_service,
                   holder::privacy::SecretStore* secret_store,
                   holder::git::GitOps* git_ops,
                   holder::llm::RunnerRegistry* runner_registry)
    : acceptor_(ioc_),
      bind_(std::move(bind)),
      port_(port),
      db_(db),
      auth_token_(auth_token),
      router_(router),
      started_at_(started_at),
      card_store_(card_store),
      fts_(fts),
      nudge_service_(nudge_service),
      secret_store_(secret_store),
      git_ops_(git_ops),
      runner_registry_(runner_registry) {}

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
  stop_requested_.store(false);
  session_workers_.clear();
  session_workers_.reserve(kInitialSessionWorkerCount);
  for (std::size_t i = 0; i < kInitialSessionWorkerCount; ++i) {
    session_workers_.emplace_back([this]() { run_session_worker(); });
  }

  while (!signals.is_requested()) {
    boost::system::error_code ec;
    tcp::socket socket(ioc_);
    acceptor_.accept(socket, ec);
    if (ec) {
      if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
        std::this_thread::sleep_for(kPollDelay);
        continue;
      }
      if (stop_requested_.load() || ec == boost::asio::error::operation_aborted ||
          ec == boost::asio::error::bad_descriptor) {
        break;
      }
      spdlog::error("accept failed: {}", ec.message());
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(session_queue_mutex_);
      if (pending_sessions_.size() >= kMaxPendingSessions) {
        spdlog::warn("session queue full; dropping accepted socket");
        boost::system::error_code close_ec;
        socket.shutdown(tcp::socket::shutdown_both, close_ec);
        socket.close(close_ec);
        continue;
      }
      pending_sessions_.emplace_back(std::move(socket));
    }
    session_queue_cv_.notify_one();
  }

  stop_requested_.store(true);
  session_queue_cv_.notify_all();
  for (auto& worker : session_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  session_workers_.clear();
  spdlog::info("listener shutdown requested");
}

void Listener::stop() {
  stop_requested_.store(true);
  session_queue_cv_.notify_all();
  boost::system::error_code ec;
  acceptor_.close(ec);
  ioc_.stop();
}

void Listener::run_session_worker() {
  while (true) {
    tcp::socket socket(ioc_);
    {
      std::unique_lock<std::mutex> lock(session_queue_mutex_);
      session_queue_cv_.wait(lock, [this]() {
        return stop_requested_.load() || !pending_sessions_.empty();
      });
      if (pending_sessions_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue;
      }
      socket = std::move(pending_sessions_.front());
      pending_sessions_.pop_front();
    }

    Session session(std::move(socket),
                    db_,
                    auth_token_,
                    router_,
                    started_at_,
                    card_store_,
                    fts_,
                    nudge_service_,
                    secret_store_,
                    git_ops_,
                    runner_registry_);
    session.run();
  }
}

} // namespace holder::api
