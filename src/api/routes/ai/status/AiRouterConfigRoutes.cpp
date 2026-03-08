#include "api/routes/ai/status/AiRouterConfigRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "ai/AiRouterConfigRepo.h"
#include "project/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes::ai::status {
namespace {

namespace http = boost::beast::http;

nlohmann::json router_config_payload(const std::optional<holder::model::AiRouterConfig>& global_cfg,
                                     const std::optional<holder::model::AiRouterConfig>& project_cfg,
                                     const std::string& project_id) {
  nlohmann::json data;
  data["global"] = {
      {"router_model", global_cfg.has_value() ? nlohmann::json(global_cfg->router_model)
                                              : nlohmann::json(nullptr)},
      {"updated_at", global_cfg.has_value() ? nlohmann::json(global_cfg->updated_at)
                                            : nlohmann::json(nullptr)},
  };
  if (!project_id.empty()) {
    data["project"] = {
        {"project_id", project_id},
        {"router_model", project_cfg.has_value() ? nlohmann::json(project_cfg->router_model)
                                                 : nlohmann::json(nullptr)},
        {"updated_at", project_cfg.has_value() ? nlohmann::json(project_cfg->updated_at)
                                               : nlohmann::json(nullptr)},
    };
  } else {
    data["project"] = nullptr;
  }

  std::string effective_scope = "auto";
  nlohmann::json effective_model = nullptr;
  if (project_cfg.has_value()) {
    effective_scope = "project";
    effective_model = project_cfg->router_model;
  } else if (global_cfg.has_value()) {
    effective_scope = "global";
    effective_model = global_cfg->router_model;
  }
  data["effective"] = {
      {"scope", effective_scope},
      {"router_model", effective_model},
  };
  return data;
}

} // namespace

bool handle_ai_router_config_routes(const std::string& path,
                                    const http::request<http::string_body>& req,
                                    http::response<http::string_body>& res,
                                    holder::platform::Db& db,
                                    const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/ai/router/config" && req.method() == http::verb::get) {
    try {
      const std::string project_id = param_get("project_id");
      holder::ai::AiRouterConfigRepo repo(db);
      const auto global_cfg = repo.get_global();
      const auto project_cfg =
          project_id.empty() ? std::optional<holder::model::AiRouterConfig>{}
                             : repo.get_for_project(project_id);

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = router_config_payload(global_cfg, project_cfg, project_id);
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path == "/ai/router/config" && req.method() == http::verb::put) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("scope")) {
        res = support::error_response(http::status::bad_request, "bad_request", "Missing scope.");
        return true;
      }

      const std::string scope = body.at("scope").get<std::string>();
      if (scope != "global" && scope != "project") {
        res = support::error_response(
            http::status::bad_request, "bad_request", "scope must be global or project.");
        return true;
      }

      bool valid = true;
      std::string project_id;
      if (scope == "project") {
        if (!body.contains("project_id") || body.at("project_id").is_null()) {
          res = support::error_response(
              http::status::bad_request, "bad_request", "project_id required for project scope.");
          valid = false;
        }
        if (valid) {
          project_id = body.at("project_id").get<std::string>();
        }
        if (valid && project_id.empty()) {
          res = support::error_response(
              http::status::bad_request, "bad_request", "project_id required for project scope.");
          valid = false;
        }
        if (valid) {
          holder::project::ProjectRepo projects(db);
          if (!projects.get(project_id).has_value()) {
            res = support::error_response(http::status::not_found, "not_found", "Project not found.");
            valid = false;
          }
        }
      }

      if (valid) {
        std::string router_model;
        bool has_model = false;
        if (body.contains("router_model") && !body.at("router_model").is_null()) {
          router_model = body.at("router_model").get<std::string>();
          has_model = !router_model.empty();
        }

        const long long updated_at =
            (body.contains("updated_at") && !body.at("updated_at").is_null())
                ? body.at("updated_at").get<long long>()
                : support::now_epoch_seconds();

        holder::ai::AiRouterConfigRepo repo(db);
        if (scope == "global") {
          if (has_model) {
            repo.set_global(router_model, updated_at);
          } else {
            repo.clear_global();
          }
        } else {
          if (has_model) {
            repo.set_for_project(project_id, router_model, updated_at);
          } else {
            repo.clear_for_project(project_id);
          }
        }

        const auto global_cfg = repo.get_global();
        const auto project_cfg = (scope == "project" && !project_id.empty())
                                     ? repo.get_for_project(project_id)
                                     : std::optional<holder::model::AiRouterConfig>{};

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = router_config_payload(global_cfg, project_cfg, project_id);
        res = support::json_response(http::status::ok, payload);
      }
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes::ai::status
