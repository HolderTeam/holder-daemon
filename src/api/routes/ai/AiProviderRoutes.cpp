#include "api/routes/ai/AiProviderRoutes.h"

#include "api/routes/ai/providers/AiProviderCatalogRoutes.h"
#include "api/routes/ai/providers/AiProviderCredentialRoutes.h"

namespace holder::api::routes {

bool handle_ai_provider_routes(const std::string& path,
                               const boost::beast::http::request<boost::beast::http::string_body>& req,
                               boost::beast::http::response<boost::beast::http::string_body>& res,
                               holder::platform::Db& db) {
  if (ai::providers::handle_ai_provider_catalog_routes(path, req, res, db)) {
    return true;
  }
  if (ai::providers::handle_ai_provider_credential_routes(path, req, res, db)) {
    return true;
  }
  return false;
}

} // namespace holder::api::routes
