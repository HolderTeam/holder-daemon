#include "api/Session.h"
#include "api/routes/AuthenticatedRoutes.h"
#include "api/routes/GoogleDriveOAuthRoutes.h"
#include "api/routes/StaticRoutes.h"
#include "api/support/HttpAuth.h"
#include "api/support/HttpResponses.h"

#include <boost/asio/bind_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <random>
#include <string>

namespace holder::api {
namespace {

namespace http = boost::beast::http;

template <typename T> void ignore_result(T&&) noexcept {}

std::string generate_uuid_v4() {
  if (const char* seed_env = std::getenv("HOLDER_UUID_SEED")) {
    try {
      const auto seed = static_cast<unsigned int>(std::stoul(seed_env));
      static std::mutex mutex;
      static std::mt19937 rng;
      static bool seeded = false;
      std::lock_guard<std::mutex> lock(mutex);
      if (!seeded) {
        rng.seed(seed);
        seeded = true;
      }
      boost::uuids::basic_random_generator<std::mt19937> gen(&rng);
      return boost::uuids::to_string(gen());
    } catch (const std::exception& ex) {
      (void)ex;
      // Fall through to random generator.
    }
  }
  boost::uuids::random_generator gen;
  return boost::uuids::to_string(gen());
}

Session::Response ping_response(const Session::Request& req) {
  if (req.method() != http::verb::get) {
    return support::error_response(
        http::status::method_not_allowed,
        "method_not_allowed",
        "Use GET for /ping."
    );
  }

  Session::Response res{http::status::ok, 11};
  res.set(http::field::content_type, "text/plain");
  res.keep_alive(false);
  res.body() = "pong";
  res.prepare_payload();
  return res;
}

struct AsyncReadResult {
  boost::system::error_code ec;
  Session::tcp::socket socket;
  Session::Request req;
};

struct AsyncWriteResult {
  boost::system::error_code ec;
  Session::tcp::socket socket;
};

AsyncReadResult async_read_request(
    Session::tcp::socket socket,
    const Session::SocketHook& on_io_start,
    const Session::SocketHook& on_io_done
) {
  namespace beast = boost::beast;
  auto promise = std::make_shared<std::promise<AsyncReadResult>>();
  auto future = promise->get_future();

  struct State {
    explicit State(Session::tcp::socket s)
        : socket(std::move(s)),
          strand(boost::asio::make_strand(socket.get_executor())) {}
    std::mutex mu;
    bool done = false;
    bool cancel_requested = false;
    Session::tcp::socket socket;
    boost::asio::strand<boost::asio::any_io_executor> strand;
    beast::flat_buffer buffer;
    Session::Request req;
  };

  auto state = std::make_shared<State>(std::move(socket));
  auto io_handle = std::make_shared<Session::IoHandle>();
  io_handle->cancel = [weak_state = std::weak_ptr<State>(state)]() {
    if (auto locked = weak_state.lock()) {
      boost::asio::strand<boost::asio::any_io_executor> strand(locked->strand);
      {
        std::lock_guard<std::mutex> lock(locked->mu);
        if (locked->done) {
          return; // LCOV_EXCL_LINE: defensive late-cancel race.
        }
        locked->cancel_requested = true;
      }
      boost::asio::post(strand, [weak_state]() {
        if (auto posted = weak_state.lock()) {
          std::lock_guard<std::mutex> lock(posted->mu);
          if (posted->done) {
            return; // LCOV_EXCL_LINE: defensive late-cancel race.
          }
          boost::system::error_code ec;
          ignore_result(posted->socket.cancel(ec)); // NOLINT(bugprone-unused-return-value)
          ignore_result(posted->socket.shutdown(Session::tcp::socket::shutdown_both, ec)
          ); // NOLINT(bugprone-unused-return-value)
          ec.clear();
          ignore_result(posted->socket.close(ec)); // NOLINT(bugprone-unused-return-value)
        }
      });
    }
  };
  if (on_io_start) {
    on_io_start(io_handle);
  }
  boost::asio::post(state->strand, [state, promise, on_io_done, io_handle]() mutable {
    http::async_read(
        state->socket,
        state->buffer,
        state->req,
        boost::asio::bind_executor(
            state->strand,
            [state,
             promise,
             on_io_done,
             io_handle](boost::system::error_code ec, std::size_t) mutable {
              std::unique_lock<std::mutex> lock(state->mu);
              state->done = true;
              AsyncReadResult result{ec, std::move(state->socket), std::move(state->req)};
              lock.unlock();
              if (on_io_done) {
                on_io_done(io_handle);
              }
              promise->set_value(std::move(result));
            }
        )
    );
  });
  return future.get();
}

AsyncWriteResult async_write_response(
    Session::PreparedResponse prepared,
    const Session::SocketHook& on_io_start,
    const Session::SocketHook& on_io_done
) {
  auto promise = std::make_shared<std::promise<AsyncWriteResult>>();
  auto future = promise->get_future();
  struct State {
    explicit State(Session::PreparedResponse prepared_response)
        : response(std::move(prepared_response)),
          strand(boost::asio::make_strand(response.socket.get_executor())) {}
    std::mutex mu;
    bool done = false;
    bool cancel_requested = false;
    Session::PreparedResponse response;
    boost::asio::strand<boost::asio::any_io_executor> strand;
  };

  auto state = std::make_shared<State>(std::move(prepared));
  auto io_handle = std::make_shared<Session::IoHandle>();
  io_handle->cancel = [weak_state = std::weak_ptr<State>(state)]() {
    if (auto locked = weak_state.lock()) {
      boost::asio::strand<boost::asio::any_io_executor> strand(locked->strand);
      {
        std::lock_guard<std::mutex> lock(locked->mu);
        if (locked->done) {
          return; // LCOV_EXCL_LINE: defensive late-cancel race.
        }
        locked->cancel_requested = true;
      }
      boost::asio::post(strand, [weak_state]() {
        if (auto posted = weak_state.lock()) {
          std::lock_guard<std::mutex> lock(posted->mu);
          if (posted->done) {
            return; // LCOV_EXCL_LINE: defensive late-cancel race.
          }
          boost::system::error_code ec;
          ignore_result(posted->response.socket.cancel(ec)); // NOLINT(bugprone-unused-return-value)
          ignore_result(posted->response.socket.shutdown(Session::tcp::socket::shutdown_both, ec)
          ); // NOLINT(bugprone-unused-return-value)
          ec.clear();
          ignore_result(posted->response.socket.close(ec)); // NOLINT(bugprone-unused-return-value)
        }
      });
    }
  };

  if (on_io_start) {
    on_io_start(io_handle);
  }
  boost::asio::post(state->strand, [state, promise, on_io_done, io_handle]() mutable {
    http::async_write(
        state->response.socket,
        state->response.res,
        boost::asio::bind_executor(
            state->strand,
            [state,
             promise,
             on_io_done,
             io_handle](boost::system::error_code ec, std::size_t) mutable {
              std::unique_lock<std::mutex> lock(state->mu);
              state->done = true;
              AsyncWriteResult result{ec, std::move(state->response.socket)};
              lock.unlock();
              if (on_io_done) {
                on_io_done(io_handle);
              }
              promise->set_value(std::move(result));
            }
        )
    );
  });
  return future.get();
}

} // namespace

Session::Session(
    tcp::socket socket,
    holder::platform::Db& db,
    const std::string& auth_token,
    const Router& router,
    std::chrono::steady_clock::time_point started_at,
    holder::card::CardStore* card_store,
    holder::index::FtsIndexer* fts,
    holder::ai::NudgeService* nudge_service,
    holder::privacy::SecretStore* secret_store,
    holder::git::GitOps* git_ops,
    holder::llm::RunnerRegistry* runner_registry
)
    : socket_(std::move(socket)),
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

Session::Session(
    PreparedRequest prepared,
    holder::platform::Db& db,
    const std::string& auth_token,
    const Router& router,
    std::chrono::steady_clock::time_point started_at,
    holder::card::CardStore* card_store,
    holder::index::FtsIndexer* fts,
    holder::ai::NudgeService* nudge_service,
    holder::privacy::SecretStore* secret_store,
    holder::git::GitOps* git_ops,
    holder::llm::RunnerRegistry* runner_registry
)
    : socket_(std::move(prepared.socket)),
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
      req_(std::move(prepared.req)),
      request_started_(prepared.request_started),
      path_(std::move(prepared.path)),
      query_string_(std::move(prepared.query_string)),
      has_loaded_request_(true) {}

std::optional<Session::PreparedRequest> Session::prepare_request(
    tcp::socket socket,
    const SocketHook& on_io_start,
    const SocketHook& on_io_done
) {
  const auto request_started = std::chrono::steady_clock::now();
  auto read_result = async_read_request(std::move(socket), on_io_start, on_io_done);
  auto& ec = read_result.ec;
  if (ec == http::error::end_of_stream) {
    ignore_result(read_result.socket.shutdown(tcp::socket::shutdown_send, ec)
    ); // NOLINT(bugprone-unused-return-value)
    return std::nullopt;
  }
  if (ec) {
    spdlog::warn("read failed: {}", ec.message());
    ignore_result(read_result.socket.shutdown(tcp::socket::shutdown_send, ec)
    ); // NOLINT(bugprone-unused-return-value)
    return std::nullopt;
  }

  const auto target = read_result.req.target();
  const std::string target_str(target.data(), target.size());
  const auto query_pos = target_str.find('?');
  const std::string path = (query_pos == std::string::npos) ? target_str
                                                            : target_str.substr(0, query_pos);
  const std::string query_string = (query_pos == std::string::npos)
                                       ? ""
                                       : target_str.substr(query_pos + 1);

  const auto lane = classify_request_lane(read_result.req, path);
  PreparedRequest prepared{
      std::move(read_result.socket),
      std::move(read_result.req),
      request_started,
      path,
      query_string,
      lane,
  };
  return prepared;
}

void Session::run() {
  auto prepared = execute();
  if (prepared.has_value()) {
    write_prepared_response(std::move(*prepared));
  }
}

std::optional<Session::PreparedResponse> Session::execute() {
  if (!ensure_request_loaded()) {
    return std::nullopt;
  }
  return process_loaded_request();
}

bool Session::ensure_request_loaded() {
  if (has_loaded_request_) {
    return true;
  }
  auto prepared = prepare_request(std::move(socket_));
  if (!prepared.has_value()) {
    return false;
  }
  socket_ = std::move(prepared->socket);
  req_ = std::move(prepared->req);
  request_started_ = prepared->request_started;
  path_ = std::move(prepared->path);
  query_string_ = std::move(prepared->query_string);
  has_loaded_request_ = true;
  return true;
}

std::optional<Session::PreparedResponse> Session::process_loaded_request() {
  Response res;
  if (path_ == "/ping") {
    res = ping_response(req_);
  } else if (!routes::handle_static_routes(path_, req_, res)) {
    // Google's redirect after the user finishes authorizing in their browser hits this
    // directly -- the browser never carries auth_token_, so this one path+method
    // combination under /locations/ must be checked (and, if matched, fully handled)
    // before the bearer-auth gate below, the same way handle_static_routes already is.
    // Everything else under /locations/, including starting the OAuth flow itself, still
    // requires the normal token. See GoogleDriveOAuthRoutes.h's own doc comments.
    if (!routes::handle_google_drive_oauth_callback_route(
            path_, query_string_, req_, res, db_, secret_store_, git_ops_
        )) {
      if (!support::is_authorized_bearer(req_, auth_token_)) {
        res = support::error_response(
            http::status::unauthorized,
            "unauthorized",
            "Missing or invalid token."
        );
      } else if (!router_.dispatch(req_, res)) {
        const auto route_result = routes::dispatch_authenticated_routes(
            path_,
            query_string_,
            req_,
            res,
            socket_,
            db_,
            card_store_,
            fts_,
            nudge_service_,
            secret_store_,
            git_ops_,
            runner_registry_,
            [&]() { // LCOV_EXCL_LINE
              return generate_uuid_v4();
            }
        );
        if (route_result.streamed) return std::nullopt;
      }
    }
  }

  PreparedResponse prepared{
      std::move(socket_),
      req_,
      std::move(res),
      request_started_,
      classify_request_lane(req_, path_),
  };
  return prepared;
}

void Session::write_prepared_response(
    PreparedResponse prepared,
    const SocketHook& on_io_start,
    const SocketHook& on_io_done
) {
  const auto method = std::string(prepared.req.method_string());
  const auto target = std::string(prepared.req.target());
  const auto status = prepared.res.result_int();
  const auto lane = prepared.lane;
  const auto request_started = prepared.request_started;

  auto write_result = async_write_response(std::move(prepared), on_io_start, on_io_done);
  auto& ec = write_result.ec;
  if (ec) {
    spdlog::warn("write failed: {}", ec.message());
  }

  const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - request_started
  )
                               .count();
  spdlog::info(
      "HTTP {} {} -> {} ({}ms)",
      method,
      target, // LCOV_EXCL_LINE
      status,
      duration_ms
  );
  spdlog::debug(
      "HTTP lane={} method={} target={} status={} duration_ms={}",
      lane_name(lane),
      method,
      target, // LCOV_EXCL_LINE
      status,
      duration_ms
  );

  ignore_result(write_result.socket.shutdown(tcp::socket::shutdown_send, ec)
  ); // NOLINT(bugprone-unused-return-value)
}

Session::RequestLane Session::classify_request_lane(const Request& req, const std::string& path) {
  // Initial route-to-lane mapping:
  // - save: card writes except link graph mutations
  // - background: AI routes, history reads, and links/backlinks refresh work
  // - foreground: everything else
  const auto method = req.method();
  const bool is_get_like = method == http::verb::get || method == http::verb::head;
  const bool is_card_route = path == "/cards" || path.rfind("/cards/", 0) == 0;
  const bool is_ai_route = path == "/ai" || path.rfind("/ai/", 0) == 0;
  const bool is_history_route = path.find("/history/") != std::string::npos;
  const bool is_link_route = path.find("/links") != std::string::npos ||
                             path.find("/backlinks") != std::string::npos;

  if (is_card_route && !is_get_like && !is_link_route) {
    return RequestLane::Save;
  }
  if (is_ai_route || is_history_route || is_link_route) {
    return RequestLane::Background;
  }
  return RequestLane::Foreground;
}

const char* Session::lane_name(RequestLane lane) {
  switch (lane) {
  case RequestLane::Save:
    return "save";
  case RequestLane::Foreground:
    return "foreground";
  case RequestLane::Background:
    return "background";
  }
  return "unknown"; // LCOV_EXCL_LINE
}

} // namespace holder::api
