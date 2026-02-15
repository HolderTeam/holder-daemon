#include "api/routes/ai/providers/AiProviderCatalogRoutes.h"

#include "api/support/CloudConfig.h"
#include "api/support/HttpResponses.h"
#include "store/AiProviderCredentialRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_set>

namespace holder::api::routes::ai::providers {
namespace {

namespace http = boost::beast::http;

nlohmann::json model_to_json(const support::CloudModelConfig& model) {
  nlohmann::json out;
  out["id"] = model.id;
  out["endpoint"] = model.endpoint;
  out["role"] = model.role.empty() ? nlohmann::json(nullptr) : nlohmann::json(model.role);
  out["limits"] = {
      {"rpm", model.rpm},
      {"tpm", model.tpm},
      {"rpd", model.rpd},
  };
  return out;
}

nlohmann::json auth_to_json(const support::CloudProviderConfig& provider) {
  nlohmann::json out = nlohmann::json::object();
  if (!provider.auth_type.empty()) out["type"] = provider.auth_type;
  if (!provider.key_param.empty()) out["key_param"] = provider.key_param;
  if (!provider.header_name.empty()) out["header_name"] = provider.header_name;
  if (!provider.bearer_prefix.empty()) out["bearer_prefix"] = provider.bearer_prefix;
  return out;
}

nlohmann::json api_to_json(const support::CloudProviderConfig& provider) {
  nlohmann::json out = nlohmann::json::object();
  if (!provider.base_url.empty()) out["base_url"] = provider.base_url;
  if (!provider.kind.empty()) out["kind"] = provider.kind;
  return out;
}

} // namespace

bool handle_ai_provider_catalog_routes(const std::string& path,
                                       const http::request<http::string_body>& req,
                                       http::response<http::string_body>& res,
                                       holder::store::Db& db) {
  if (path != "/ai/providers/catalog" || req.method() != http::verb::get) {
    return false;
  }

  try {
    const auto cloud_cfg = support::load_cloudproviders_config();
    if (!cloud_cfg.has_value()) {
      res = support::error_response(http::status::bad_request,
                                    "bad_request",
                                    "cloudproviders.yaml not found.");
      return true;
    }

    holder::store::AiProviderCredentialRepo credential_repo(db);
    std::unordered_set<std::string> configured_ids;
    for (const auto& credential : credential_repo.list()) {
      configured_ids.insert(credential.provider);
    }

    nlohmann::json data;
    data["default_provider"] = cloud_cfg->default_provider.empty()
                                   ? nlohmann::json(nullptr)
                                   : nlohmann::json(cloud_cfg->default_provider);

    nlohmann::json providers = nlohmann::json::array();
    for (const auto& provider : cloud_cfg->providers) {
      nlohmann::json item;
      item["id"] = provider.id;
      item["display_name"] = provider.display_name.empty() ? provider.id : provider.display_name;
      item["enabled"] = provider.enabled;
      item["configured"] = configured_ids.find(provider.id) != configured_ids.end();
      item["api"] = api_to_json(provider);
      item["auth"] = auth_to_json(provider);
      nlohmann::json models = nlohmann::json::array();
      for (const auto& model : provider.models) {
        models.push_back(model_to_json(model));
      }
      item["models"] = models;
      providers.push_back(std::move(item));
    }
    data["providers"] = providers;

    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = data;
    res = support::json_response(http::status::ok, payload);
    return true;
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    return true;
  }
}

} // namespace holder::api::routes::ai::providers
