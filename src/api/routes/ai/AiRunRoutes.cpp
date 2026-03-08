#include "api/routes/ai/AiRunRoutes.h"

#include "api/routes/ai/runs/AiRunPostRoute.h"
#include "api/routes/ai/runs/AiRunQueryRoutes.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

} // namespace

RouteDispatchResult handle_ai_run_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get) {
  RouteDispatchResult out{};

  if (path == "/ai/runs" && req.method() == http::verb::post) {
    return ai::runs::handle_ai_runs_post_route(req, res, socket, db, fts, runner, uuid_v4);
  }

  if (path == "/ai/runs" && req.method() == http::verb::get) {
    return ai::runs::handle_ai_runs_list_route(param_get, res, db);
  }

  if (path.rfind("/ai/runs/", 0) == 0 &&
      path.size() > std::string("/ai/runs/").size() + std::string("/events").size() &&
      path.compare(path.size() - std::string("/events").size(),
                   std::string("/events").size(),
                   "/events") == 0 &&
      req.method() == http::verb::get) {
    return ai::runs::handle_ai_runs_events_route(path, socket, res, db);
  }

  if (path.rfind("/ai/runs/", 0) == 0 && req.method() == http::verb::get) {
    return ai::runs::handle_ai_runs_get_route(path, res, db);
  }

  return out;
}

} // namespace holder::api::routes
