#include "api/Session.h"
#include "api/routes/AiMessageRoutes.h"
#include "api/routes/AiResourceRoutes.h"
#include "api/routes/AiRunnerRoutes.h"
#include "api/routes/AiThreadRoutes.h"
#include "api/routes/CardRoutes.h"
#include "api/routes/ProjectRoutes.h"
#include "api/routes/RebuildRoutes.h"
#include "api/routes/SearchRoutes.h"
#include "api/routes/TrashRoutes.h"
#include "api/routes/StaticRoutes.h"
#include "api/routes/AiRunRoutes.h"
#include "api/routes/AiStatusRoutes.h"
#include "api/routes/AiProviderRoutes.h"
#include "api/support/PathDiscovery.h"
#include "api/support/CloudConfig.h"
#include "api/support/CloudClient.h"
#include "api/support/CloudQuota.h"
#include "api/support/LocalModelRouting.h"
#include "api/support/RunEventStore.h"

#include "caste.hpp"
#include "core/CardPaths.h"
#include "core/ProjectPaths.h"
#include "git/GitOps.h"
#include "llm/LocalModelRunner.h"
#include "core/ServerInfo.h"
#include "store/AiProviderCredentialRepo.h"
#include "store/AiRouterConfigRepo.h"
#include "store/AiRunRepo.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace holder::api {
namespace {

namespace http = boost::beast::http;

bool is_authorized(const http::request<http::string_body>& req, const std::string& token) {
  const auto it = req.find(http::field::authorization);
  if (it == req.end()) return false;

  const auto value = it->value();
  const std::string auth(value.data(), value.size());
  constexpr char kPrefix[] = "Bearer ";
  if (auth.rfind(kPrefix, 0) != 0) return false;

  const std::string bearer = auth.substr(sizeof(kPrefix) - 1);
  return bearer == token;
}

http::response<http::string_body> json_response(http::status status,
                                                const nlohmann::json& payload) {
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, "application/json");
  res.keep_alive(false);
  res.body() = payload.dump();
  res.prepare_payload();
  return res;
}

http::response<http::string_body> error_response(http::status status,
                                                 std::string code,
                                                 std::string message) {
  nlohmann::json j;
  j["ok"] = false;
  j["error"] = {{"code", std::move(code)}, {"message", std::move(message)}};
  return json_response(status, j);
}

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
                 holder::store::Db& db,
                 const std::string& auth_token,
                 const Router& router,
                 std::chrono::steady_clock::time_point started_at,
                 holder::store::CardStore* card_store,
                 holder::index::FtsIndexer* fts,
                 holder::git::GitOps* git_ops,
                 holder::llm::LocalModelRunner* runner)
    : socket_(std::move(socket)),
      db_(db),
      auth_token_(auth_token),
      router_(router),
      started_at_(started_at),
      card_store_(card_store),
      fts_(fts),
      git_ops_(git_ops),
      runner_(runner) {}

void Session::run() {
  namespace beast = boost::beast;
  namespace http = boost::beast::http;

  beast::flat_buffer buffer;
  http::request<http::string_body> req;
  boost::system::error_code ec;
  const auto request_started = std::chrono::steady_clock::now();

  http::read(socket_, buffer, req, ec);
  if (ec == http::error::end_of_stream) {
    socket_.shutdown(tcp::socket::shutdown_send, ec);
    return;
  }
  if (ec) {
    spdlog::warn("read failed: {}", ec.message());
    socket_.shutdown(tcp::socket::shutdown_send, ec);
    return;
  }

  const auto target = req.target();
  const std::string target_str(target.data(), target.size());

  const auto query_pos = target_str.find('?');
  const std::string path = (query_pos == std::string::npos)
                               ? target_str
                               : target_str.substr(0, query_pos);
  const std::string query_string =
      (query_pos == std::string::npos) ? "" : target_str.substr(query_pos + 1);

  http::response<http::string_body> res;

  if (routes::handle_static_routes(path, req, res)) {
    // handled
  } else if (!is_authorized(req, auth_token_)) {
    res = error_response(http::status::unauthorized, "unauthorized", "Missing or invalid token.");
  } else if (router_.dispatch(req, res)) {
    // handled
  } else {
    auto param = [&](const std::string& key) -> std::string {
      const std::string needle = key + "=";
      const auto pos = query_string.find(needle);
      if (pos == std::string::npos) return {};
      const auto start = pos + needle.size();
      const auto end = query_string.find('&', start);
      return query_string.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    if (routes::handle_project_routes(path,
                                      req,
                                      res,
                                      db_,
                                      git_ops_,
                                      [&]() { return generate_uuid_v4(); },
                                      param)) {
      // handled
    } else if (routes::handle_rebuild_routes(path, req, res, db_, fts_)) {
      // handled
    } else if (routes::handle_search_routes(path, req, res, fts_, param)) {
      // handled
    } else if (routes::handle_ai_status_routes(path, req, res, db_, runner_, param)) {
      // handled
    } else if (routes::handle_ai_provider_routes(path, req, res, db_)) {
      // handled
    } else if (const auto route_result = routes::handle_ai_run_routes(path,
                                                                      req,
                                                                      res,
                                                                      socket_,
                                                                      db_,
                                                                      fts_,
                                                                      runner_,
                                                                      [&]() { return generate_uuid_v4(); },
                                                                      param);
               route_result.handled) {
      if (route_result.streamed) return;
    } else if (const auto runner_route_result =
                   routes::handle_ai_runner_routes(path, req, res, socket_, runner_);
               runner_route_result.handled) {
      if (runner_route_result.streamed) return;
    } else if (routes::handle_ai_thread_routes(path,
                                               req,
                                               res,
                                               db_,
                                               [&]() { return generate_uuid_v4(); },
                                               param)) {
      // handled
    } else if (routes::handle_ai_message_routes(path,
                                                req,
                                                res,
                                                db_,
                                                fts_,
                                                [&]() { return generate_uuid_v4(); },
                                                param)) {
      // handled
    } else if (routes::handle_ai_resource_routes(path,
                                                 req,
                                                 res,
                                                 db_,
                                                 [&]() { return generate_uuid_v4(); },
                                                 param)) {
      // handled
    } else if (routes::handle_trash_routes(path, req, res, db_, card_store_, fts_, param)) {
      // handled
    } else if (routes::handle_card_routes(path,
                                          req,
                                          res,
                                          db_,
                                          card_store_,
                                          fts_,
                                          [&]() { return generate_uuid_v4(); },
                                          param)) {
      // handled
    } else {
      res = error_response(http::status::not_found, "not_found", "Route not found.");
    }
  }

  http::write(socket_, res, ec);
  if (ec) {
    spdlog::warn("write failed: {}", ec.message());
  }

  const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - request_started)
                               .count();
  spdlog::info("HTTP {} {} -> {} ({}ms)",
               req.method_string(),
               req.target(),
               res.result_int(),
               duration_ms);

  socket_.shutdown(tcp::socket::shutdown_send, ec);
}

} // namespace holder::api
