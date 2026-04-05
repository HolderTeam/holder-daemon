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

std::optional<Session::PreparedRequest> Session::prepare_request(tcp::socket socket) {
  namespace beast = boost::beast;

  beast::flat_buffer buffer;
  Request req;
  boost::system::error_code ec;
  const auto request_started = std::chrono::steady_clock::now();

  http::read(socket, buffer, req, ec);
  if (ec == http::error::end_of_stream) {
    socket.shutdown(tcp::socket::shutdown_send, ec);
    return std::nullopt;
  }
  if (ec) {
    spdlog::warn("read failed: {}", ec.message());
    socket.shutdown(tcp::socket::shutdown_send, ec);
    return std::nullopt;
  }

  const auto target = req.target();
  const std::string target_str(target.data(), target.size());
  const auto query_pos = target_str.find('?');
  const std::string path = (query_pos == std::string::npos)
                               ? target_str
                               : target_str.substr(0, query_pos);
  const std::string query_string =
      (query_pos == std::string::npos) ? "" : target_str.substr(query_pos + 1);

  const auto lane = classify_request_lane(req, path);
  PreparedRequest prepared{
      std::move(socket),
      std::move(req),
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

void Session::write_prepared_response(PreparedResponse prepared) {
  boost::system::error_code ec;
  http::write(prepared.socket, prepared.res, ec);
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

  prepared.socket.shutdown(tcp::socket::shutdown_send, ec);
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
