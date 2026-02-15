#include "api/routes/ai/AiMessageRoutes.h"

#include "api/routes/ai/messages/AiMessageCaptureRoutes.h"
#include "api/routes/ai/messages/AiMessageCrudRoutes.h"
#include "api/routes/ai/messages/AiMessageLinkRoutes.h"

namespace holder::api::routes {

bool handle_ai_message_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::store::Db& db,
    holder::index::FtsIndexer* fts,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get) {
  if (ai::messages::handle_ai_message_capture_routes(path, req, res, db, fts, uuid_v4)) {
    return true;
  }
  if (ai::messages::handle_ai_message_link_routes(path, req, res, db, fts, param_get)) {
    return true;
  }
  if (ai::messages::handle_ai_message_crud_routes(path, req, res, db, fts, uuid_v4, param_get)) {
    return true;
  }
  return false;
}

} // namespace holder::api::routes
