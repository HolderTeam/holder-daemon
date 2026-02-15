#include "api/routes/AiProviderRoutes.h"
#include "api/support/HttpResponses.h"
#include "api/support/ProviderUtils.h"
#include "api/support/Time.h"

#include "api/support/CloudConfig.h"
#include "store/AiProviderCredentialRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <unordered_set>

namespace holder::api::routes {
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

bool handle_ai_provider_routes(const std::string& path,
                               const http::request<http::string_body>& req,
                               http::response<http::string_body>& res,
                               holder::store::Db& db) {
  if (path == "/ai/providers/catalog" && req.method() == http::verb::get) {
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

  if (path == "/ai/providers/credentials" && req.method() == http::verb::get) {
    try {
      holder::store::AiProviderCredentialRepo repo(db);
      nlohmann::json providers = nlohmann::json::array();
      for (const auto& credential : repo.list()) {
        providers.push_back({
            {"provider", credential.provider},
            {"configured", true},
            {"api_key_preview", support::mask_api_key(credential.api_key)},
            {"created_at", credential.created_at},
            {"updated_at", credential.updated_at},
        });
      }
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {{"providers", providers}};
      res = support::json_response(http::status::ok, payload);
      return true;
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      return true;
    }
  }

  if (path == "/ai/providers/credentials" && req.method() == http::verb::put) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("provider") || !body.contains("api_key") ||
          body.at("provider").is_null() || body.at("api_key").is_null()) {
        res = support::error_response(http::status::bad_request,
                             "bad_request",
                             "Missing provider or api_key.");
        return true;
      }

      const std::string provider =
          support::normalize_provider_name(body.at("provider").get<std::string>());
      if (provider.empty()) {
        res = support::error_response(http::status::bad_request,
                             "bad_request",
                             "provider must be alphanumeric and may include '-', '_' or '.'.");
        return true;
      }

      const std::string api_key = support::trim_ascii(body.at("api_key").get<std::string>());
      if (api_key.empty()) {
        res = support::error_response(http::status::bad_request,
                             "bad_request",
                             "api_key cannot be empty.");
        return true;
      }

      const long long ts =
          (body.contains("updated_at") && !body.at("updated_at").is_null())
              ? body.at("updated_at").get<long long>()
              : support::now_epoch_seconds();

      holder::store::AiProviderCredentialRepo repo(db);
      const auto existing = repo.get(provider);
      const long long created_at = existing.has_value() ? existing->created_at : ts;
      repo.upsert(provider, api_key, created_at, ts);

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {
          {"provider", provider},
          {"configured", true},
          {"api_key_preview", support::mask_api_key(api_key)},
          {"created_at", created_at},
          {"updated_at", ts},
      };
      res = support::json_response(http::status::ok, payload);
      return true;
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      return true;
    }
  }

  if (path.rfind("/ai/providers/credentials/", 0) == 0 &&
      req.method() == http::verb::delete_) {
    try {
      const std::string provider = support::normalize_provider_name(
          path.substr(std::string("/ai/providers/credentials/").size()));
      if (provider.empty()) {
        res = support::error_response(http::status::bad_request,
                             "bad_request",
                             "Invalid provider.");
        return true;
      }
      holder::store::AiProviderCredentialRepo repo(db);
      repo.remove(provider);
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {{"provider", provider}};
      res = support::json_response(http::status::ok, payload);
      return true;
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      return true;
    }
  }

  return false;
}

} // namespace holder::api::routes
