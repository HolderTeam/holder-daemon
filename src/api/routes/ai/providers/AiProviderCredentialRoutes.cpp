#include "api/routes/ai/providers/AiProviderCredentialRoutes.h"

#include "ai/AiProviderCredentialRepo.h"
#include "ai/AiProviderSettingRepo.h"
#include "platform/DeviceConfigStore.h"
#include "api/support/HttpResponses.h"
#include "api/support/ProviderUtils.h"
#include "api/support/Time.h"
#include "privacy/SecretStore.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace holder::api::routes::ai::providers {
namespace {

namespace http = boost::beast::http;

std::string preview_for_output(const std::string& stored) {
  return stored.find('*') != std::string::npos ? stored : support::mask_api_key(stored);
}

} // namespace

bool handle_ai_provider_credential_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::privacy::SecretStore& secret_store
) {
  static constexpr const char* kSecretService = "holder.ai_provider_credentials";
  if (path == "/ai/providers/settings" && req.method() == http::verb::get) {
    try {
      holder::ai::AiProviderSettingRepo repo(db);
      nlohmann::json providers = nlohmann::json::array();
      for (const auto& setting : repo.list()) {
        providers.push_back({
            {"provider", setting.provider},
            {"enabled", setting.enabled},
            {"updated_at", setting.updated_at},
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

  if (path == "/ai/providers/settings" && req.method() == http::verb::put) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("provider") || !body.contains("enabled") ||
          body.at("provider").is_null() || body.at("enabled").is_null()) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "Missing provider or enabled."
        );
        return true;
      }

      const std::string provider = support::normalize_provider_name(
          body.at("provider").get<std::string>()
      );
      if (provider.empty()) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "provider must be alphanumeric and may include '-', '_' or '.'."
        );
        return true;
      }
      const bool enabled = body.at("enabled").get<bool>();
      const long long ts = (body.contains("updated_at") && !body.at("updated_at").is_null())
                               ? body.at("updated_at").get<long long>()
                               : support::now_epoch_seconds();

      holder::ai::AiProviderSettingRepo repo(db);
      repo.upsert(provider, enabled, ts);
      holder::core::persist_device_config(db);

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {
          {"provider", provider},
          {"enabled", enabled},
          {"updated_at", ts},
      };
      res = support::json_response(http::status::ok, payload);
      return true;
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      return true;
    }
  }

  if (path.rfind("/ai/providers/settings/", 0) == 0 && req.method() == http::verb::delete_) {
    try {
      const std::string provider = support::normalize_provider_name(
          path.substr(std::string("/ai/providers/settings/").size())
      );
      if (provider.empty()) {
        res =
            support::error_response(http::status::bad_request, "bad_request", "Invalid provider.");
        return true;
      }
      holder::ai::AiProviderSettingRepo repo(db);
      repo.remove(provider);
      holder::core::persist_device_config(db);
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

  if (path == "/ai/providers/credentials" && req.method() == http::verb::get) {
    try {
      holder::ai::AiProviderCredentialRepo repo(db);
      nlohmann::json providers = nlohmann::json::array();
      for (const auto& credential : repo.list()) {
        providers.push_back({
            {"provider", credential.provider},
            {"configured", true},
            {"api_key_preview", preview_for_output(credential.api_key_preview)},
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
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "Missing provider or api_key."
        );
        return true;
      }

      const std::string provider = support::normalize_provider_name(
          body.at("provider").get<std::string>()
      );
      if (provider.empty()) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "provider must be alphanumeric and may include '-', '_' or '.'."
        );
        return true;
      }

      const std::string api_key = support::trim_ascii(body.at("api_key").get<std::string>());
      if (api_key.empty()) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "api_key cannot be empty."
        );
        return true;
      }

      const long long ts = (body.contains("updated_at") && !body.at("updated_at").is_null())
                               ? body.at("updated_at").get<long long>()
                               : support::now_epoch_seconds();
      const std::string api_key_preview = support::mask_api_key(api_key);

      holder::ai::AiProviderCredentialRepo repo(db);
      const auto existing = repo.get(provider);
      const long long created_at = existing.has_value() ? existing->created_at : ts;
      secret_store.set(kSecretService, provider, api_key, api_key_preview, created_at, ts);
      repo.upsert(provider, api_key_preview, created_at, ts);
      holder::ai::AiProviderSettingRepo setting_repo(db);
      setting_repo.upsert(provider, true, ts);
      holder::core::persist_device_config(db);

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {
          {"provider", provider},
          {"configured", true},
          {"api_key_preview", api_key_preview},
          {"created_at", created_at},
          {"updated_at", ts},
      };
      res = support::json_response(http::status::ok, payload);
      return true;
    } catch (const holder::privacy::PrivacyError& ex) {
      res = support::error_response(
          http::status::service_unavailable,
          holder::privacy::privacy_error_code_name(ex.code()),
          ex.what()
      );
      return true;
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      return true;
    }
  }

  if (path.rfind("/ai/providers/credentials/", 0) == 0 && req.method() == http::verb::delete_) {
    try {
      const std::string provider = support::normalize_provider_name(
          path.substr(std::string("/ai/providers/credentials/").size())
      );
      if (provider.empty()) {
        res =
            support::error_response(http::status::bad_request, "bad_request", "Invalid provider.");
        return true;
      }
      secret_store.remove(kSecretService, provider);
      holder::ai::AiProviderCredentialRepo repo(db);
      repo.remove(provider);
      holder::ai::AiProviderSettingRepo setting_repo(db);
      setting_repo.upsert(provider, false, support::now_epoch_seconds());
      holder::core::persist_device_config(db);
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {{"provider", provider}};
      res = support::json_response(http::status::ok, payload);
      return true;
    } catch (const holder::privacy::PrivacyError& ex) {
      res = support::error_response(
          http::status::service_unavailable,
          holder::privacy::privacy_error_code_name(ex.code()),
          ex.what()
      );
      return true;
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      return true;
    }
  }

  return false;
}

} // namespace holder::api::routes::ai::providers
