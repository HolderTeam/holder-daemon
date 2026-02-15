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
#include "api/support/HttpQuery.h"
#include "api/support/HttpResponses.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

std::string first_segment(const std::string& path) {
  if (path.empty() || path[0] != '/') return {};
  const auto start = std::size_t{1};
  if (start >= path.size()) return {};
  const auto end = path.find('/', start);
  return path.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

std::string segment_at(const std::string& path, std::size_t index) {
  if (path.empty() || path[0] != '/') return {};
  std::size_t cursor = 1;
  std::size_t current = 1;
  while (cursor <= path.size()) {
    const auto end = path.find('/', cursor);
    const auto seg_end = (end == std::string::npos) ? path.size() : end;
    if (current == index) {
      if (cursor >= seg_end) return {};
      return path.substr(cursor, seg_end - cursor);
    }
    if (end == std::string::npos) break;
    cursor = end + 1;
    ++current;
  }
  return {};
}

struct AiDispatchResult {
  bool handled = false;
  bool streamed = false;
};

AiDispatchResult dispatch_ai_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::store::Db& db,
    holder::index::FtsIndexer* fts,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param) {
  const std::string ai_resource = segment_at(path, 2);

  if (ai_resource == "capabilities" || ai_resource == "status" || ai_resource == "retry" ||
      ai_resource == "router") {
    if (handle_ai_status_routes(path, req, res, db, runner, param)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "providers") {
    if (handle_ai_provider_routes(path, req, res, db)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "runs") {
    if (const auto route_result =
            handle_ai_run_routes(path, req, res, socket, db, fts, runner, uuid_v4, param);
        route_result.handled) {
      return {.handled = true, .streamed = route_result.streamed};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "runner") {
    if (const auto route_result = handle_ai_runner_routes(path, req, res, socket, runner);
        route_result.handled) {
      return {.handled = true, .streamed = route_result.streamed};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "threads") {
    if (handle_ai_thread_routes(path, req, res, db, uuid_v4, param)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "messages") {
    if (handle_ai_message_routes(path, req, res, db, fts, uuid_v4, param)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }

  return {.handled = false, .streamed = false};
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
    return support::query_param_value(query_string, key);
  };

  const std::string resource = first_segment(path);

  if (resource == "projects") {
    if (handle_project_routes(path, req, res, db, git_ops, uuid_v4, param)) return {};
  } else if (resource == "rebuild") {
    if (handle_rebuild_routes(path, req, res, db, fts)) return {};
  } else if (resource == "search") {
    if (handle_search_routes(path, req, res, fts, param)) return {};
  } else if (resource == "ai") {
    const auto route_result =
        dispatch_ai_routes(path, req, res, socket, db, fts, runner, uuid_v4, param);
    if (route_result.handled) {
      return {.streamed = route_result.streamed};
    }
  } else if (resource == "resources") {
    if (handle_ai_resource_routes(path, req, res, db, uuid_v4, param)) return {};
  } else if (resource == "trash") {
    if (handle_trash_routes(path, req, res, db, card_store, fts, param)) return {};
  } else if (resource == "cards") {
    if (handle_card_routes(path, req, res, db, card_store, fts, uuid_v4, param)) return {};
  }

  res = support::error_response(http::status::not_found, "not_found", "Route not found.");
  return {};
}

} // namespace holder::api::routes
