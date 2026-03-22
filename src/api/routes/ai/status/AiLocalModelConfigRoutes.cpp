#include "api/routes/ai/status/AiLocalModelConfigRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "ai/AiLocalModelConfigRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes::ai::status {
namespace {

namespace http = boost::beast::http;

nlohmann::json config_payload(const std::optional<holder::model::AiLocalModelConfig>& cfg) {
  return {
      {"fast_model", cfg.has_value() && cfg->fast_model.has_value()
                         ? nlohmann::json(cfg->fast_model.value())
                         : nlohmann::json(nullptr)},
      {"strong_model", cfg.has_value() && cfg->strong_model.has_value()
                           ? nlohmann::json(cfg->strong_model.value())
                           : nlohmann::json(nullptr)},
      {"deep_model", cfg.has_value() && cfg->deep_model.has_value()
                         ? nlohmann::json(cfg->deep_model.value())
                         : nlohmann::json(nullptr)},
      {"updated_at", cfg.has_value() ? nlohmann::json(cfg->updated_at) : nlohmann::json(nullptr)},
  };
}

std::optional<std::string> read_optional_model(const nlohmann::json& body, const char* key) {
  if (!body.contains(key) || body.at(key).is_null()) {
    return std::nullopt;
  }
  auto value = body.at(key).get<std::string>();
  if (value.empty()) {
    return std::nullopt;
  }
  return value;
}

} // namespace

bool handle_ai_local_model_config_routes(const std::string& path,
                                         const http::request<http::string_body>& req,
                                         http::response<http::string_body>& res,
                                         holder::platform::Db& db) {
  if (path == "/ai/local-models/config" && req.method() == http::verb::get) {
    try {
      holder::ai::AiLocalModelConfigRepo repo(db);
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = config_payload(repo.get());
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path == "/ai/local-models/config" && req.method() == http::verb::put) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      const auto fast_model = read_optional_model(body, "fast_model");
      const auto strong_model = read_optional_model(body, "strong_model");
      const auto deep_model = read_optional_model(body, "deep_model");
      const long long updated_at =
          (body.contains("updated_at") && !body.at("updated_at").is_null())
              ? body.at("updated_at").get<long long>()
              : support::now_epoch_seconds();

      holder::ai::AiLocalModelConfigRepo repo(db);
      repo.set(fast_model, strong_model, deep_model, updated_at);

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = config_payload(repo.get());
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes::ai::status
