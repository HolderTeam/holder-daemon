#include "api/routes/ai/AiThreadRoutes.h"

#include "api/routes/ai/threads/AiThreadCollectionRoutes.h"
#include "api/routes/ai/threads/AiThreadItemRoutes.h"

namespace holder::api::routes {

bool handle_ai_thread_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get
) {
  if (ai::threads::handle_ai_thread_collection_routes(path, req, res, db, uuid_v4, param_get)) {
    return true;
  }
  if (ai::threads::handle_ai_thread_item_routes(path, req, res, db)) {
    return true;
  }
  return false;
}

} // namespace holder::api::routes
