#include "api/Listener.h"

#include "api/support/HttpResponses.h"

#include <boost/beast/http.hpp>
#include <memory>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

namespace holder::api {
namespace {

namespace http = boost::beast::http;

constexpr auto kPollDelay = std::chrono::milliseconds(50);
constexpr std::size_t kIngressWorkerCount = 1;
constexpr std::size_t kReservedSaveWorkerCount = 1;
constexpr std::size_t kGeneralWorkerCount = 3;
constexpr std::size_t kWriterWorkerCount = 1;
constexpr std::size_t kMaxPendingAcceptedSockets = 64;
constexpr std::size_t kMaxPreparedRequestsPerLane = 64;

// Queue contract:
// - accepted sockets wait here for request read/parse/classification
// - save_queue: highest priority card-write work only
// - foreground_queue: user-visible non-save work
// - background_queue: AI, nudges, links/backlinks, and other best-effort work
// - response_queue: completed non-streaming responses waiting for socket write
//
// Admission rules:
// - accepted sockets are dropped when the ingress queue is full
// - prepared requests receive 503 when their target lane queue is full
// - reserved save workers consume save_queue only
// - general workers consume foreground_queue, then background_queue
// - background work never runs on reserved save workers
//
// Initial worker counts stay conservative until more shared subsystems are audited.

void reject_prepared_request(Session::PreparedRequest prepared,
                             http::status status,
                             const std::string& code,
                             const std::string& message) {
  boost::system::error_code ec;
  auto res = support::error_response(status, code, message);
  http::write(prepared.socket, res, ec);
  prepared.socket.shutdown(Session::tcp::socket::shutdown_send, ec);
}

struct WorkerContext {
  holder::platform::Db db;
  std::unique_ptr<holder::index::FtsIndexer> fts;
  std::unique_ptr<holder::card::CardStore> card_store;
  std::unique_ptr<holder::llm::RunnerRegistry> runner_registry;
  std::unique_ptr<holder::ai::NudgeService> nudge_service;
};

WorkerContext make_worker_context(holder::platform::Db& root_db,
                                  holder::index::FtsIndexer* root_fts,
                                  holder::card::CardStore* root_card_store,
                                  holder::llm::RunnerRegistry* root_runner_registry,
                                  holder::ai::NudgeService* root_nudge_service,
                                  holder::git::GitOps* git_ops,
                                  const holder::core::SerialExecutor* ai_runtime_executor) {
  WorkerContext context;
  context.db.open(root_db.path());

  if (root_fts != nullptr) {
    context.fts = std::make_unique<holder::index::FtsIndexer>(context.db);
  }
  if (root_card_store != nullptr) {
    context.card_store = std::make_unique<holder::card::CardStore>(
        context.db,
        context.fts.get(),
        nullptr,
        git_ops);
  }

  holder::llm::RunnerClient* auto_local_client = nullptr;
  if (root_runner_registry != nullptr) {
    auto_local_client =
        root_runner_registry->get_client(holder::llm::RunnerRegistry::kAutoLocalRunnerId);
    context.runner_registry = std::make_unique<holder::llm::RunnerRegistry>(
        &context.db,
        auto_local_client,
        ai_runtime_executor);
  }

  if (root_nudge_service != nullptr) {
    context.nudge_service = std::make_unique<holder::ai::NudgeService>(
        context.db,
        context.runner_registry.get());
  }

  return context;
}

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
  owned_git_ops_.reset();
  git_executor_.reset();
  executor_git_ops_.reset();
  ai_runtime_executor_.reset();
  request_git_ops_ = git_ops_;

  if (card_store_ != nullptr || request_git_ops_ != nullptr) {
    if (request_git_ops_ == nullptr) {
      owned_git_ops_ = std::make_unique<holder::git::RealGitOps>();
      request_git_ops_ = owned_git_ops_.get();
    }
    git_executor_ = std::make_unique<holder::core::SerialExecutor>("request-git");
    executor_git_ops_ =
        std::make_unique<holder::git::ExecutorGitOps>(*request_git_ops_, *git_executor_);
    request_git_ops_ = executor_git_ops_.get();
  }
  if (runner_registry_ != nullptr) {
    ai_runtime_executor_ = std::make_unique<holder::core::SerialExecutor>("request-ai-runtime");
  }

  ingress_workers_.clear();
  save_workers_.clear();
  general_workers_.clear();
  writer_workers_.clear();

  ingress_workers_.reserve(kIngressWorkerCount);
  for (std::size_t i = 0; i < kIngressWorkerCount; ++i) {
    ingress_workers_.emplace_back([this]() { run_ingress_worker(); });
  }

  save_workers_.reserve(kReservedSaveWorkerCount);
  for (std::size_t i = 0; i < kReservedSaveWorkerCount; ++i) {
    save_workers_.emplace_back([this]() { run_save_worker(); });
  }

  general_workers_.reserve(kGeneralWorkerCount);
  for (std::size_t i = 0; i < kGeneralWorkerCount; ++i) {
    general_workers_.emplace_back([this]() { run_general_worker(); });
  }

  writer_workers_.reserve(kWriterWorkerCount);
  for (std::size_t i = 0; i < kWriterWorkerCount; ++i) {
    writer_workers_.emplace_back([this]() { run_writer_worker(); });
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
      std::lock_guard<std::mutex> lock(ingress_queue_mutex_);
      if (pending_sockets_.size() >= kMaxPendingAcceptedSockets) {
        spdlog::warn("accepted socket queue full; dropping socket");
        boost::system::error_code close_ec;
        socket.shutdown(tcp::socket::shutdown_both, close_ec);
        socket.close(close_ec);
        continue;
      }
      pending_sockets_.emplace_back(std::move(socket));
    }
    ingress_queue_cv_.notify_one();
  }

  stop_requested_.store(true);
  ingress_queue_cv_.notify_all();
  lane_queue_cv_.notify_all();
  response_queue_cv_.notify_all();

  for (auto& worker : ingress_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  for (auto& worker : save_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  for (auto& worker : general_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  for (auto& worker : writer_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  ingress_workers_.clear();
  save_workers_.clear();
  general_workers_.clear();
  writer_workers_.clear();
  spdlog::info("listener shutdown requested");
}

void Listener::stop() {
  stop_requested_.store(true);
  ingress_queue_cv_.notify_all();
  lane_queue_cv_.notify_all();
  response_queue_cv_.notify_all();
  boost::system::error_code ec;
  acceptor_.close(ec);
  ioc_.stop();
}

void Listener::run_ingress_worker() {
  while (true) {
    tcp::socket socket(ioc_);
    {
      std::unique_lock<std::mutex> lock(ingress_queue_mutex_);
      ingress_queue_cv_.wait(lock, [this]() {
        return stop_requested_.load() || !pending_sockets_.empty();
      });
      if (pending_sockets_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue;
      }
      socket = std::move(pending_sockets_.front());
      pending_sockets_.pop_front();
    }

    auto prepared = Session::prepare_request(std::move(socket));
    if (!prepared.has_value()) {
      continue;
    }

    bool queued = false;
    {
      std::lock_guard<std::mutex> lock(lane_queue_mutex_);
      auto* target_queue = &foreground_queue_;
      if (prepared->lane == Session::RequestLane::Save) {
        target_queue = &save_queue_;
      } else if (prepared->lane == Session::RequestLane::Background) {
        target_queue = &background_queue_;
      }

      if (target_queue->size() < kMaxPreparedRequestsPerLane) {
        target_queue->emplace_back(std::move(*prepared));
        queued = true;
      }
    }

    if (!queued) {
      spdlog::warn("request lane queue full; rejecting prepared request");
      reject_prepared_request(std::move(*prepared),
                              http::status::service_unavailable,
                              "server_busy",
                              "Server busy. Please retry.");
      continue;
    }
    lane_queue_cv_.notify_all();
  }
}

void Listener::run_save_worker() {
  auto context =
      make_worker_context(db_,
                          fts_,
                          card_store_,
                          runner_registry_,
                          nudge_service_,
                          request_git_ops_,
                          ai_runtime_executor_.get());

  while (true) {
    Session::PreparedRequest prepared{tcp::socket(ioc_), {}, {}, "", "", Session::RequestLane::Save};
    {
      std::unique_lock<std::mutex> lock(lane_queue_mutex_);
      lane_queue_cv_.wait(lock, [this]() {
        return stop_requested_.load() || !save_queue_.empty();
      });
      if (save_queue_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue;
      }
      prepared = std::move(save_queue_.front());
      save_queue_.pop_front();
    }

    Session session(std::move(prepared),
                    context.db,
                    auth_token_,
                    router_,
                    started_at_,
                    context.card_store.get(),
                    context.fts.get(),
                    context.nudge_service.get(),
                    secret_store_,
                    git_ops_,
                    context.runner_registry.get());
    auto response = session.execute();
    if (response.has_value()) {
      {
        std::lock_guard<std::mutex> lock(response_queue_mutex_);
        response_queue_.emplace_back(std::move(*response));
      }
      response_queue_cv_.notify_one();
    }
  }
}

void Listener::run_general_worker() {
  auto context =
      make_worker_context(db_,
                          fts_,
                          card_store_,
                          runner_registry_,
                          nudge_service_,
                          request_git_ops_,
                          ai_runtime_executor_.get());

  while (true) {
    Session::PreparedRequest prepared{
        tcp::socket(ioc_), {}, {}, "", "", Session::RequestLane::Foreground};
    {
      std::unique_lock<std::mutex> lock(lane_queue_mutex_);
      lane_queue_cv_.wait(lock, [this]() {
        return stop_requested_.load() || !foreground_queue_.empty() || !background_queue_.empty();
      });
      if (foreground_queue_.empty() && background_queue_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue;
      }

      if (!foreground_queue_.empty()) {
        prepared = std::move(foreground_queue_.front());
        foreground_queue_.pop_front();
      } else {
        prepared = std::move(background_queue_.front());
        background_queue_.pop_front();
      }
    }

    Session session(std::move(prepared),
                    context.db,
                    auth_token_,
                    router_,
                    started_at_,
                    context.card_store.get(),
                    context.fts.get(),
                    context.nudge_service.get(),
                    secret_store_,
                    git_ops_,
                    context.runner_registry.get());
    auto response = session.execute();
    if (response.has_value()) {
      {
        std::lock_guard<std::mutex> lock(response_queue_mutex_);
        response_queue_.emplace_back(std::move(*response));
      }
      response_queue_cv_.notify_one();
    }
  }
}

void Listener::run_writer_worker() {
  while (true) {
    Session::PreparedResponse prepared{tcp::socket(ioc_), {}, {}, {}};
    {
      std::unique_lock<std::mutex> lock(response_queue_mutex_);
      response_queue_cv_.wait(lock, [this]() {
        return stop_requested_.load() || !response_queue_.empty();
      });
      if (response_queue_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue;
      }
      prepared = std::move(response_queue_.front());
      response_queue_.pop_front();
    }
    Session::write_prepared_response(std::move(prepared));
  }
}

} // namespace holder::api
