#include "api/routes/AuthenticatedRoutes.h"

#include "api/routes/AiResourceRoutes.h"
#include "api/routes/CardRoutes.h"
#include "api/routes/ProjectRoutes.h"
#include "api/routes/RebuildRoutes.h"
#include "api/routes/SearchRoutes.h"
#include "api/routes/TrashRoutes.h"
#include "api/routes/ai/AiDispatch.h"
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
  } else if (resource == "recovery-token") {
    if (handle_project_routes(path, req, res, db, git_ops, uuid_v4, param)) return {};
  } else if (resource == "rebuild") {
    if (handle_rebuild_routes(path, req, res, db, fts)) return {};
  } else if (resource == "search") {
    if (handle_search_routes(path, req, res, fts, param)) return {};
  } else if (resource == "ai") {
    const auto route_result =
        ai::dispatch_ai_routes(path, req, res, socket, db, fts, runner, uuid_v4, param);
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
