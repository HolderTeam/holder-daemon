#include "api/Listener.h"

#include "api/support/HttpResponses.h"

#include <boost/beast/http.hpp>
#include <memory>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <thread>

namespace holder::api {
namespace {

namespace http = boost::beast::http;

constexpr auto kPollDelay = std::chrono::milliseconds(50);
constexpr std::size_t kMaxPendingAcceptedSockets = 64;
constexpr std::size_t kMaxPreparedRequestsPerLane = 64;

template <typename T> void ignore_result(T&&) noexcept {}

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

bool client_disconnected(Session::tcp::socket& socket) {
  if (!socket.is_open()) {
    return true; // LCOV_EXCL_LINE
  }

  boost::system::error_code ec;
  const bool was_non_blocking = socket.non_blocking();
  ignore_result(socket.non_blocking(true, ec)); // NOLINT(bugprone-unused-return-value)
  if (ec) {
    return false; // LCOV_EXCL_LINE
  }

  std::array<char, 1> buf{};
  const auto received =
      socket.receive(boost::asio::buffer(buf), boost::asio::socket_base::message_peek, ec);

  boost::system::error_code restore_ec;
  ignore_result(socket.non_blocking(was_non_blocking, restore_ec)
  ); // NOLINT(bugprone-unused-return-value)

  if (!ec) {
    return received == 0; // LCOV_EXCL_LINE
  }
  if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
    return false;
  }
  return ec == boost::asio::error::eof || ec == boost::asio::error::connection_reset ||
         ec == boost::asio::error::bad_descriptor; // LCOV_EXCL_LINE
} // LCOV_EXCL_LINE

bool should_drop_stale_background_request(Session::PreparedRequest& prepared) {
  if (prepared.lane != Session::RequestLane::Background) {
    return false;
  }
  return client_disconnected(prepared.socket);
}

void reject_prepared_request(
    Session::PreparedRequest prepared,
    http::status status,
    const std::string& code,
    const std::string& message
) {
  boost::system::error_code ec;
  auto res = support::error_response(status, code, message);
  http::write(prepared.socket, res, ec); // NOLINT(bugprone-unused-return-value)
  ignore_result(prepared.socket.shutdown(Session::tcp::socket::shutdown_send, ec)
  ); // NOLINT(bugprone-unused-return-value)
}

struct WorkerContext {
  holder::platform::Db db;
  std::unique_ptr<holder::index::FtsIndexer> fts;
  std::unique_ptr<holder::card::CardStore> card_store;
  std::unique_ptr<holder::llm::RunnerRegistry> runner_registry;
  std::unique_ptr<holder::ai::NudgeService> nudge_service;
};

WorkerContext make_worker_context(
    holder::platform::Db& root_db,
    holder::index::FtsIndexer* root_fts,
    holder::card::CardStore* root_card_store,
    holder::llm::RunnerRegistry* root_runner_registry,
    holder::ai::NudgeService* root_nudge_service,
    holder::git::GitOps* git_ops,
    const holder::core::SerialExecutor* ai_runtime_executor
) {
  WorkerContext context;
  context.db.open(root_db.path());

  if (root_fts != nullptr) {
    context.fts = std::make_unique<holder::index::FtsIndexer>(context.db);
  }
  if (root_card_store != nullptr) {
    context.card_store =
        std::make_unique<holder::card::CardStore>(context.db, context.fts.get(), nullptr, git_ops);
  }

  holder::llm::RunnerClient* auto_local_client = nullptr;
  if (root_runner_registry != nullptr) {
    auto_local_client = root_runner_registry->get_client(
        holder::llm::RunnerRegistry::kAutoLocalRunnerId
    );
    context.runner_registry = std::make_unique<holder::llm::RunnerRegistry>(
        &context.db,
        auto_local_client,
        ai_runtime_executor
    );
  }

  if (root_nudge_service != nullptr) {
    context.nudge_service =
        std::make_unique<holder::ai::NudgeService>(context.db, context.runner_registry.get());
  }

  return context;
} // LCOV_EXCL_LINE

void track_socket(
    std::mutex& mutex,
    std::unordered_set<Session::IoHandlePtr>& sockets,
    const Session::IoHandlePtr& socket
) {
  std::lock_guard<std::mutex> lock(mutex);
  if (socket != nullptr) {
    sockets.insert(socket);
  }
}

void untrack_socket(
    std::mutex& mutex,
    std::unordered_set<Session::IoHandlePtr>& sockets,
    const Session::IoHandlePtr& socket
) {
  std::lock_guard<std::mutex> lock(mutex);
  if (socket != nullptr) {
    sockets.erase(socket);
  }
}

} // namespace

Listener::Listener(
    std::string bind,
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
    holder::llm::RunnerRegistry* runner_registry,
    holder::api::ConcurrencyProfile concurrency
)
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
      runner_registry_(runner_registry),
      concurrency_(concurrency) {}

Listener::BoundInfo Listener::start() {
  boost::system::error_code ec;
  const auto address = boost::asio::ip::make_address(bind_, ec);
  if (ec) {
    throw std::runtime_error("Invalid bind address: " + bind_ + " (" + ec.message() + ")");
  }

  tcp::endpoint endpoint(address, port_);
  ignore_result(acceptor_.open(endpoint.protocol(), ec)); // NOLINT(bugprone-unused-return-value)
  if (ec) throw std::runtime_error("acceptor.open failed: " + ec.message());

  ignore_result(acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec)
  ); // NOLINT(bugprone-unused-return-value)
  if (ec) throw std::runtime_error("acceptor.set_option failed: " + ec.message());

  ignore_result(acceptor_.bind(endpoint, ec)); // NOLINT(bugprone-unused-return-value)
  if (ec) throw std::runtime_error("acceptor.bind failed: " + ec.message());

  ignore_result(acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec)
  ); // NOLINT(bugprone-unused-return-value)
  if (ec) throw std::runtime_error("acceptor.listen failed: " + ec.message());

  ignore_result(acceptor_.non_blocking(true, ec)); // NOLINT(bugprone-unused-return-value)
  if (ec) throw std::runtime_error("acceptor.non_blocking failed: " + ec.message());

  const auto bound = acceptor_.local_endpoint(ec);
  if (ec) throw std::runtime_error("acceptor.local_endpoint failed: " + ec.message());

  return BoundInfo{bound.address().to_string(), bound.port()};
}

void Listener::run(const holder::core::SignalHandler& signals) {
  stop_requested_.store(false);
  ioc_.restart();
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
    git_executor_ = std::make_unique<holder::core::SerialExecutor>("request-git", 16);
    executor_git_ops_ =
        std::make_unique<holder::git::ExecutorGitOps>(*request_git_ops_, *git_executor_);
    request_git_ops_ = executor_git_ops_.get();
  }
  if (runner_registry_ != nullptr) {
    ai_runtime_executor_ = std::make_unique<holder::core::SerialExecutor>("request-ai-runtime", 16);
  }

  ingress_workers_.clear();
  save_workers_.clear();
  general_workers_.clear();
  writer_workers_.clear();
  io_workers_.clear();

  ingress_workers_.reserve(concurrency_.ingress_workers);
  for (std::size_t i = 0; i < concurrency_.ingress_workers; ++i) {
    ingress_workers_.emplace_back([this]() {
      run_ingress_worker();
    });
  }

  save_workers_.reserve(concurrency_.save_workers);
  for (std::size_t i = 0; i < concurrency_.save_workers; ++i) {
    save_workers_.emplace_back([this]() {
      run_save_worker();
    });
  }

  general_workers_.reserve(concurrency_.general_workers);
  for (std::size_t i = 0; i < concurrency_.general_workers; ++i) {
    general_workers_.emplace_back([this]() {
      run_general_worker();
    });
  }

  writer_workers_.reserve(concurrency_.writer_workers);
  for (std::size_t i = 0; i < concurrency_.writer_workers; ++i) {
    writer_workers_.emplace_back([this]() {
      run_writer_worker();
    });
  }

  start_accept_loop();
  io_workers_.reserve(concurrency_.io_threads);
  for (std::size_t i = 0; i < concurrency_.io_threads; ++i) {
    io_workers_.emplace_back([this]() {
      ioc_.run();
    });
  }

  while (!stop_requested_.load() && !signals.is_requested()) {
    std::this_thread::sleep_for(kPollDelay);
  }

  stop();

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

  ioc_.stop();
  for (auto& worker : io_workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  ingress_workers_.clear();
  save_workers_.clear();
  general_workers_.clear();
  writer_workers_.clear();
  io_workers_.clear();
  spdlog::info("listener shutdown requested");
}

void Listener::stop() {
  bool expected = false;
  if (!stop_requested_.compare_exchange_strong(expected, true)) {
    return;
  }

  shutdown_queued_work();
  shutdown_active_sockets();
  ingress_queue_cv_.notify_all();
  lane_queue_cv_.notify_all();
  response_queue_cv_.notify_all();
  boost::system::error_code ec;
  ignore_result(acceptor_.close(ec)); // NOLINT(bugprone-unused-return-value)
}

std::size_t Listener::active_read_socket_count() const {
  std::lock_guard<std::mutex> lock(active_socket_mutex_);
  return active_read_sockets_.size();
}

std::size_t Listener::pending_socket_count() const {
  std::lock_guard<std::mutex> lock(ingress_queue_mutex_);
  return pending_sockets_.size();
}

std::size_t Listener::save_queue_count() const {
  std::lock_guard<std::mutex> lock(lane_queue_mutex_);
  return save_queue_.size();
}

std::size_t Listener::response_queue_count() const {
  std::lock_guard<std::mutex> lock(response_queue_mutex_);
  return response_queue_.size();
}

std::size_t Listener::background_queue_count() const {
  std::lock_guard<std::mutex> lock(lane_queue_mutex_);
  return background_queue_.size();
}

void Listener::enqueue_pending_socket_for_test() {
  std::lock_guard<std::mutex> lock(ingress_queue_mutex_);
  pending_sockets_.emplace_back(ioc_);
  ingress_queue_cv_.notify_one();
}

void Listener::start_accept_loop() {
  if (stop_requested_.load()) {
    return; // LCOV_EXCL_LINE
  }

  acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) mutable {
    if (ec) {
      if (stop_requested_.load() || ec == boost::asio::error::operation_aborted ||
          ec == boost::asio::error::bad_descriptor) {
        return;
      }
      spdlog::error("accept failed: {}", ec.message()); // LCOV_EXCL_LINE
      if (!stop_requested_.load()) { // LCOV_EXCL_LINE
        start_accept_loop(); // LCOV_EXCL_LINE
      } // LCOV_EXCL_LINE
      return; // LCOV_EXCL_LINE
    }

    // LCOV_EXCL_START: stop-after-accept depends on shutdown timing.
    if (stop_requested_.load()) {
      boost::system::error_code close_ec;
      ignore_result(socket.shutdown(tcp::socket::shutdown_both, close_ec)
      ); // NOLINT(bugprone-unused-return-value)
      ignore_result(socket.close(close_ec)); // NOLINT(bugprone-unused-return-value)
      return;
    }
    // LCOV_EXCL_STOP

    {
      std::lock_guard<std::mutex> lock(ingress_queue_mutex_);
      if (pending_sockets_.size() >= kMaxPendingAcceptedSockets) {
        spdlog::warn("accepted socket queue full; dropping socket");
        boost::system::error_code close_ec;
        ignore_result(socket.shutdown(tcp::socket::shutdown_both, close_ec)
        ); // NOLINT(bugprone-unused-return-value)
        ignore_result(socket.close(close_ec)); // NOLINT(bugprone-unused-return-value)
      } else {
        pending_sockets_.emplace_back(std::move(socket));
        ingress_queue_cv_.notify_one();
      }
    }

    if (!stop_requested_.load()) {
      start_accept_loop();
    }
  });
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
        continue; // LCOV_EXCL_LINE
      }
      socket = std::move(pending_sockets_.front());
      pending_sockets_.pop_front();
    }

    if (stop_requested_.load()) {
      close_socket(socket); // LCOV_EXCL_LINE
      continue; // LCOV_EXCL_LINE
    }

    auto prepared = Session::prepare_request(
        std::move(socket),
        [this](const Session::IoHandlePtr& active) { // LCOV_EXCL_LINE
          track_socket(active_socket_mutex_, active_read_sockets_, active);
        },
        [this](const Session::IoHandlePtr& active) { // LCOV_EXCL_LINE
          untrack_socket(active_socket_mutex_, active_read_sockets_, active);
        }
    );
    if (!prepared.has_value()) {
      continue;
    }
    if (stop_requested_.load()) {
      close_socket(prepared->socket); // LCOV_EXCL_LINE
      continue; // LCOV_EXCL_LINE
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
        spdlog::debug(
            "queued request lane={} target={} queue_size_before={}",
            Session::lane_name(prepared->lane),
            std::string(prepared->req.target()),
            target_queue->size()
        );
        target_queue->emplace_back(std::move(*prepared));
        queued = true;
      }
    }

    if (!queued) {
      spdlog::warn(
          "request lane queue full; rejecting lane={} target={}",
          Session::lane_name(prepared->lane),
          std::string(prepared->req.target())
      );
      reject_prepared_request(
          std::move(*prepared),
          http::status::service_unavailable,
          "server_busy",
          "Server busy. Please retry."
      );
      continue;
    }
    lane_queue_cv_.notify_all();
  }
}

void Listener::run_save_worker() {
  auto context = make_worker_context(
      db_,
      fts_,
      card_store_,
      runner_registry_,
      nudge_service_,
      request_git_ops_,
      ai_runtime_executor_.get()
  );

  while (true) {
    Session::PreparedRequest
        prepared{tcp::socket(ioc_), {}, {}, "", "", Session::RequestLane::Save};
    {
      std::unique_lock<std::mutex> lock(lane_queue_mutex_);
      lane_queue_cv_.wait(lock, [this]() {
        return stop_requested_.load() || !save_queue_.empty();
      });
      if (save_queue_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue; // LCOV_EXCL_LINE
      }
      prepared = std::move(save_queue_.front());
      save_queue_.pop_front();
    }

    Session session(
        std::move(prepared),
        context.db,
        auth_token_,
        router_,
        started_at_,
        context.card_store.get(),
        context.fts.get(),
        context.nudge_service.get(),
        secret_store_,
        git_ops_,
        context.runner_registry.get()
    );
    auto response = session.execute();
    if (response.has_value()) {
      if (stop_requested_.load()) {
        close_socket(response->socket); // LCOV_EXCL_LINE
        continue; // LCOV_EXCL_LINE
      }
      {
        std::lock_guard<std::mutex> lock(response_queue_mutex_);
        response_queue_.emplace_back(std::move(*response));
      }
      response_queue_cv_.notify_one();
    }
  }
}

void Listener::run_general_worker() {
  auto context = make_worker_context(
      db_,
      fts_,
      card_store_,
      runner_registry_,
      nudge_service_,
      request_git_ops_,
      ai_runtime_executor_.get()
  );

  while (true) {
    Session::PreparedRequest
        prepared{tcp::socket(ioc_), {}, {}, "", "", Session::RequestLane::Foreground};
    {
      std::unique_lock<std::mutex> lock(lane_queue_mutex_);
      lane_queue_cv_.wait(lock, [this]() {
        return stop_requested_.load() || !foreground_queue_.empty() || !background_queue_.empty();
      });
      if (foreground_queue_.empty() && background_queue_.empty()) {
        if (stop_requested_.load()) {
          return;
        }
        continue; // LCOV_EXCL_LINE
      }

      if (!foreground_queue_.empty()) {
        prepared = std::move(foreground_queue_.front());
        foreground_queue_.pop_front();
      } else {
        prepared = std::move(background_queue_.front());
        background_queue_.pop_front();
      }
    }

    if (should_drop_stale_background_request(prepared)) {
      spdlog::info(
          "dropping stale background request before execution: lane={} target={}",
          Session::lane_name(prepared.lane),
          std::string(prepared.req.target())
      );
      boost::system::error_code close_ec;
      ignore_result(prepared.socket.shutdown(tcp::socket::shutdown_both, close_ec)
      ); // NOLINT(bugprone-unused-return-value)
      ignore_result(prepared.socket.close(close_ec)); // NOLINT(bugprone-unused-return-value)
      continue;
    }

    Session session(
        std::move(prepared),
        context.db,
        auth_token_,
        router_,
        started_at_,
        context.card_store.get(),
        context.fts.get(),
        context.nudge_service.get(),
        secret_store_,
        git_ops_,
        context.runner_registry.get()
    );
    auto response = session.execute();
    if (response.has_value()) {
      if (stop_requested_.load()) {
        close_socket(response->socket);
        continue; // LCOV_EXCL_LINE
      }
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
        continue; // LCOV_EXCL_LINE
      }
      prepared = std::move(response_queue_.front());
      response_queue_.pop_front();
    }
    Session::write_prepared_response(
        std::move(prepared),
        [this](const Session::IoHandlePtr& active) {
          track_socket(active_socket_mutex_, active_write_sockets_, active);
        },
        [this](const Session::IoHandlePtr& active) {
          untrack_socket(active_socket_mutex_, active_write_sockets_, active);
        }
    );
  }
}

void Listener::close_socket(tcp::socket& socket) {
  boost::system::error_code ec;
  ignore_result(socket.cancel(ec)); // NOLINT(bugprone-unused-return-value)
  ignore_result(socket.shutdown(tcp::socket::shutdown_both, ec)
  ); // NOLINT(bugprone-unused-return-value)
  ignore_result(socket.close(ec)); // NOLINT(bugprone-unused-return-value)
}

void Listener::shutdown_queued_work() {
  {
    std::lock_guard<std::mutex> lock(ingress_queue_mutex_);
    for (auto& socket : pending_sockets_) {
      close_socket(socket);
    }
    pending_sockets_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(lane_queue_mutex_);
    for (auto& prepared : save_queue_) {
      close_socket(prepared.socket); // LCOV_EXCL_LINE
    }
    for (auto& prepared : foreground_queue_) {
      close_socket(prepared.socket); // LCOV_EXCL_LINE
    }
    for (auto& prepared : background_queue_) {
      close_socket(prepared.socket);
    }
    save_queue_.clear();
    foreground_queue_.clear();
    background_queue_.clear();
  }

  {
    std::lock_guard<std::mutex> lock(response_queue_mutex_);
    for (auto& prepared : response_queue_) {
      close_socket(prepared.socket);
    }
    response_queue_.clear();
  }
}

void Listener::shutdown_active_sockets() {
  std::vector<Session::IoHandlePtr> sockets_to_cancel;
  {
    std::lock_guard<std::mutex> lock(active_socket_mutex_);
    sockets_to_cancel.reserve(active_read_sockets_.size() + active_write_sockets_.size());
    sockets_to_cancel
        .insert(sockets_to_cancel.end(), active_read_sockets_.begin(), active_read_sockets_.end());
    sockets_to_cancel.insert(
        sockets_to_cancel.end(),
        active_write_sockets_.begin(),
        active_write_sockets_.end()
    );
  }

  for (const auto& io_handle : sockets_to_cancel) {
    if (io_handle != nullptr && io_handle->cancel) {
      io_handle->cancel();
    }
  }
}

} // namespace holder::api
