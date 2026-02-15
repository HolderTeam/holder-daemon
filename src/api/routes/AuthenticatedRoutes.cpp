#include "api/routes/AuthenticatedRoutes.h"

#include "api/routes/AiMessageRoutes.h"
#include "api/routes/AiProviderRoutes.h"
#include "api/routes/AiResourceRoutes.h"
#include "api/routes/AiRunnerRoutes.h"
#include "api/routes/AiRunRoutes.h"
#include "api/routes/AiStatusRoutes.h"
#include "api/routes/AiThreadRoutes.h"
#include "api/routes/CardRoutes.h"
#include "api/routes/ProjectRoutes.h"
#include "api/routes/RebuildRoutes.h"
#include "api/routes/SearchRoutes.h"
#include "api/routes/TrashRoutes.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

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

} // namespace

AuthenticatedDispatchResult dispatch_authenticated_routes(
    const std::string& path,
    const std::string& query_string,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::store::Db& db,
    holder::store::CardStore* card_store,
    holder::index::FtsIndexer* fts,
    holder::git::GitOps* git_ops,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4) {
  auto param = [&](const std::string& key) -> std::string {
    const std::string needle = key + "=";
    const auto pos = query_string.find(needle);
    if (pos == std::string::npos) return {};
    const auto start = pos + needle.size();
    const auto end = query_string.find('&', start);
    return query_string.substr(start, end == std::string::npos ? std::string::npos : end - start);
  };

  if (handle_project_routes(path, req, res, db, git_ops, uuid_v4, param)) {
    return {};
  }
  if (handle_rebuild_routes(path, req, res, db, fts)) {
    return {};
  }
  if (handle_search_routes(path, req, res, fts, param)) {
    return {};
  }
  if (handle_ai_status_routes(path, req, res, db, runner, param)) {
    return {};
  }
  if (handle_ai_provider_routes(path, req, res, db)) {
    return {};
  }
  if (const auto route_result =
          handle_ai_run_routes(path, req, res, socket, db, fts, runner, uuid_v4, param);
      route_result.handled) {
    return {.streamed = route_result.streamed};
  }
  if (const auto route_result = handle_ai_runner_routes(path, req, res, socket, runner);
      route_result.handled) {
    return {.streamed = route_result.streamed};
  }
  if (handle_ai_thread_routes(path, req, res, db, uuid_v4, param)) {
    return {};
  }
  if (handle_ai_message_routes(path, req, res, db, fts, uuid_v4, param)) {
    return {};
  }
  if (handle_ai_resource_routes(path, req, res, db, uuid_v4, param)) {
    return {};
  }
  if (handle_trash_routes(path, req, res, db, card_store, fts, param)) {
    return {};
  }
  if (handle_card_routes(path, req, res, db, card_store, fts, uuid_v4, param)) {
    return {};
  }

  res = error_response(http::status::not_found, "not_found", "Route not found.");
  return {};
}

} // namespace holder::api::routes
