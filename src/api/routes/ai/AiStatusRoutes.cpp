#include "api/routes/ai/AiStatusRoutes.h"

#include "api/routes/ai/status/AiCapabilitiesRoutes.h"
#include "api/routes/ai/status/AiRouterConfigRoutes.h"
#include "api/routes/ai/status/AiRuntimeStatusRoutes.h"

namespace holder::api::routes {

bool handle_ai_status_routes(const std::string& path,
                             const boost::beast::http::request<boost::beast::http::string_body>& req,
                             boost::beast::http::response<boost::beast::http::string_body>& res,
                             holder::platform::Db& db,
                             holder::llm::LocalModelRunner* runner,
                             const std::function<std::string(const std::string&)>& param_get) {
  if (ai::status::handle_ai_capabilities_routes(path, req, res, db, runner, param_get)) {
    return true;
  }
  if (ai::status::handle_ai_runtime_status_routes(path, req, res, db, runner)) {
    return true;
  }
  if (ai::status::handle_ai_router_config_routes(path, req, res, db, param_get)) {
    return true;
  }
  return false;
}

} // namespace holder::api::routes
