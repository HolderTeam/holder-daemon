#include "api/Session.h"
#include "api/routes/AuthenticatedRoutes.h"
#include "api/routes/StaticRoutes.h"
#include "api/support/HttpAuth.h"
#include "api/support/HttpResponses.h"

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
    } catch (const std::exception&) {
      // Fall through to random generator.
    }
  }
  boost::uuids::random_generator gen;
  return boost::uuids::to_string(gen());
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

AsyncReadResult async_read_request(Session::tcp::socket socket,
                                   const Session::SocketHook& on_io_start,
                                   const Session::SocketHook& on_io_done) {
  namespace beast = boost::beast;
  auto promise = std::make_shared<std::promise<AsyncReadResult>>();
  auto future = promise->get_future();

  struct State {
    explicit State(Session::tcp::socket s) : socket(std::move(s)) {}
    Session::tcp::socket socket;
    beast::flat_buffer buffer;
    Session::Request req;
  };

  auto state = std::make_shared<State>(std::move(socket));
  if (on_io_start) {
    on_io_start(&state->socket);
  }
  http::async_read(state->socket,
                   state->buffer,
                   state->req,
                   [state, promise, on_io_done](boost::system::error_code ec, std::size_t) mutable {
                     if (on_io_done) {
                       on_io_done(&state->socket);
                     }
                     promise->set_value(
                         AsyncReadResult{ec, std::move(state->socket), std::move(state->req)});
                   });
  return future.get();
}

AsyncWriteResult async_write_response(Session::PreparedResponse prepared,
                                      const Session::SocketHook& on_io_start,
                                      const Session::SocketHook& on_io_done) {
  auto promise = std::make_shared<std::promise<AsyncWriteResult>>();
  auto future = promise->get_future();
  auto state = std::make_shared<Session::PreparedResponse>(std::move(prepared));

  if (on_io_start) {
    on_io_start(&state->socket);
  }
  http::async_write(state->socket,
                    state->res,
                    [state, promise, on_io_done](boost::system::error_code ec, std::size_t) mutable {
                      if (on_io_done) {
                        on_io_done(&state->socket);
                      }
                      promise->set_value(AsyncWriteResult{ec, std::move(state->socket)});
                    });
  return future.get();
}

} // namespace

Session::Session(tcp::socket socket,
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

Session::Session(PreparedRequest prepared,
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

std::optional<Session::PreparedRequest> Session::prepare_request(tcp::socket socket,
                                                                 SocketHook on_io_start,
                                                                 SocketHook on_io_done) {
  const auto request_started = std::chrono::steady_clock::now();
  auto read_result = async_read_request(std::move(socket), on_io_start, on_io_done);
  auto& ec = read_result.ec;
  if (ec == http::error::end_of_stream) {
    read_result.socket.shutdown(tcp::socket::shutdown_send, ec);
    return std::nullopt;
  }
  if (ec) {
    spdlog::warn("read failed: {}", ec.message());
    read_result.socket.shutdown(tcp::socket::shutdown_send, ec);
    return std::nullopt;
  }

  const auto target = read_result.req.target();
  const std::string target_str(target.data(), target.size());
  const auto query_pos = target_str.find('?');
  const std::string path = (query_pos == std::string::npos)
                               ? target_str
                               : target_str.substr(0, query_pos);
  const std::string query_string =
      (query_pos == std::string::npos) ? "" : target_str.substr(query_pos + 1);

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
  if (routes::handle_static_routes(path_, req_, res)) {
    // handled
  } else if (!support::is_authorized_bearer(req_, auth_token_)) {
    res = support::error_response(http::status::unauthorized,
                                  "unauthorized",
                                  "Missing or invalid token.");
  } else if (router_.dispatch(req_, res)) {
    // handled
  } else {
    const auto route_result =
        routes::dispatch_authenticated_routes(path_,
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
                                              [&]() { return generate_uuid_v4(); });
    if (route_result.streamed) return std::nullopt;
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

void Session::write_prepared_response(PreparedResponse prepared,
                                      SocketHook on_io_start,
                                      SocketHook on_io_done) {
  auto write_result =
      async_write_response(std::move(prepared), on_io_start, on_io_done);
  auto& ec = write_result.ec;
  if (ec) {
    spdlog::warn("write failed: {}", ec.message());
  }

  const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - prepared.request_started)
                               .count();
  spdlog::info("HTTP {} {} -> {} ({}ms)",
               prepared.req.method_string(),
               prepared.req.target(), // LCOV_EXCL_LINE
               prepared.res.result_int(),
               duration_ms);
  spdlog::debug("HTTP lane={} method={} target={} status={} duration_ms={}",
                lane_name(prepared.lane),
                prepared.req.method_string(),
                prepared.req.target(),
                prepared.res.result_int(),
                duration_ms);

  write_result.socket.shutdown(tcp::socket::shutdown_send, ec);
}

Session::RequestLane Session::classify_request_lane(const Request& req,
                                                    const std::string& path) {
  // Initial route-to-lane mapping:
  // - save: card writes except link graph mutations
  // - background: AI routes and links/backlinks refresh work
  // - foreground: everything else
  const auto method = req.method();
  const bool is_get_like = method == http::verb::get || method == http::verb::head;
  const bool is_card_route = path == "/cards" || path.rfind("/cards/", 0) == 0;
  const bool is_ai_route = path == "/ai" || path.rfind("/ai/", 0) == 0;
  const bool is_link_route =
      path.find("/links") != std::string::npos || path.find("/backlinks") != std::string::npos;

  if (is_card_route && !is_get_like && !is_link_route) {
    return RequestLane::Save;
  }
  if (is_ai_route || is_link_route) {
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
  return "unknown";
}

} // namespace holder::api
