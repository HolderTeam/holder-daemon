#include "api/routes/RebuildRoutes.h"
#include "api/support/HttpResponses.h"

#include "store/ProjectRepo.h"
#include "store/Rebuilder.h"

#include <boost/beast/http.hpp>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

} // namespace

bool handle_rebuild_routes(const std::string& path,
                           const http::request<http::string_body>& req,
                           http::response<http::string_body>& res,
                           holder::store::Db& db,
                           holder::index::FtsIndexer* fts) {
  if (path != "/rebuild" || req.method() != http::verb::post) {
    return false;
  }

  try {
    const auto body = nlohmann::json::parse(req.body());
    if (!body.contains("project_id")) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      return true;
    }

    const std::string project_id = body.at("project_id").get<std::string>();
    holder::store::ProjectRepo repo(db);
    const auto project_opt = repo.get(project_id);
    if (!project_opt.has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "Project not found.");
      return true;
    }

    holder::store::Rebuilder rebuilder(db, fts);
    const auto stats = rebuilder.rebuild_project(project_opt.value());
    nlohmann::json data;
    data["project_id"] = project_id;
    data["cards"] = stats.cards;
    data["ai_messages"] = stats.ai_messages;
    data["ai_threads"] = stats.ai_threads;
    data["links"] = stats.links;
    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = data;
    res = support::json_response(http::status::ok, payload);
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  }

  return true;
}

} // namespace holder::api::routes
