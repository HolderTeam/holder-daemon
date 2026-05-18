#include "api/routes/ai/providers/AiProviderCatalogRoutes.h"

#include "ai/AiProviderCredentialRepo.h"
#include "ai/AiProviderSettingRepo.h"
#include "api/support/CloudConfig.h"
#include "api/support/HttpResponses.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace holder::api::routes::ai::providers {
namespace {

namespace http = boost::beast::http;

nlohmann::json model_to_json(const support::CloudModelConfig& model) {
  nlohmann::json out;
  out["id"] = model.id;
  out["endpoint"] = model.endpoint;
  out["role"] = model.role.empty() ? nlohmann::json(nullptr) : nlohmann::json(model.role);
  out["cost_tier"] = model.cost_tier.empty() ? nlohmann::json(nullptr)
                                             : nlohmann::json(model.cost_tier);
  out["default_for_low_budget"] = model.default_for_low_budget;
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
  return out; // LCOV_EXCL_LINE
} // LCOV_EXCL_LINE

nlohmann::json api_to_json(const support::CloudProviderConfig& provider) {
  nlohmann::json out = nlohmann::json::object();
  if (!provider.base_url.empty()) out["base_url"] = provider.base_url;
  if (!provider.kind.empty()) out["kind"] = provider.kind;
  return out; // LCOV_EXCL_LINE
} // LCOV_EXCL_LINE

} // namespace

bool handle_ai_provider_catalog_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db
) {
  if (path != "/ai/providers/catalog" || req.method() != http::verb::get) {
    return false;
  }

  try {
    const auto cloud_cfg = support::load_cloudproviders_config();
    if (!cloud_cfg.has_value()) {
      res = support::error_response(
          http::status::bad_request,
          "bad_request",
          "ai_catalog.yaml models runtime/catalog not found."
      );
      return true;
    }

    holder::ai::AiProviderCredentialRepo credential_repo(db);
    std::unordered_set<std::string> configured_ids;
    for (const auto& credential : credential_repo.list()) {
      configured_ids.insert(credential.provider);
    }
    holder::ai::AiProviderSettingRepo setting_repo(db);
    std::unordered_map<std::string, bool> enabled_by_provider;
    for (const auto& setting : setting_repo.list()) {
      enabled_by_provider[setting.provider] = setting.enabled;
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
      const auto enabled_it = enabled_by_provider.find(provider.id);
      item["enabled"] = (enabled_it != enabled_by_provider.end()) ? enabled_it->second
                                                                  : provider.enabled;
      item["configured"] = configured_ids.find(provider.id) != configured_ids.end();
      item["cost_tier"] = provider.cost_tier.empty() ? nlohmann::json(nullptr)
                                                     : nlohmann::json(provider.cost_tier);
      item["setup_url"] = provider.setup_url.empty() ? nlohmann::json(nullptr)
                                                     : nlohmann::json(provider.setup_url);
      item["docs_url"] = provider.docs_url.empty() ? nlohmann::json(nullptr)
                                                   : nlohmann::json(provider.docs_url);
      item["api_key_label"] = provider.api_key_label.empty()
                                  ? nlohmann::json(nullptr)
                                  : nlohmann::json(provider.api_key_label);
      item["api_key_hint"] = provider.api_key_hint.empty() ? nlohmann::json(nullptr)
                                                           : nlohmann::json(provider.api_key_hint);
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
