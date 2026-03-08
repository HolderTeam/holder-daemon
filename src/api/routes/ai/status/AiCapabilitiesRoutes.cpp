#include "api/routes/ai/status/AiCapabilitiesRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/LocalModelRouting.h"
#include "api/support/Time.h"
#include "ai/AiRouterConfigRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes::ai::status {
namespace {

namespace http = boost::beast::http;

} // namespace

bool handle_ai_capabilities_routes(const std::string& path,
                                   const http::request<http::string_body>& req,
                                   http::response<http::string_body>& res,
                                   holder::store::Db& db,
                                   holder::llm::LocalModelRunner* runner,
                                   const std::function<std::string(const std::string&)>& param_get) {
  if (path != "/ai/capabilities" || req.method() != http::verb::get) {
    return false;
  }

  nlohmann::json data;
  const std::string project_id = param_get("project_id");
  std::optional<holder::model::AiRouterConfig> global_router_cfg;
  std::optional<holder::model::AiRouterConfig> project_router_cfg;
  try {
    holder::ai::AiRouterConfigRepo router_cfg_repo(db);
    global_router_cfg = router_cfg_repo.get_global();
    project_router_cfg = project_id.empty() ? std::optional<holder::model::AiRouterConfig>{}
                                            : router_cfg_repo.get_for_project(project_id);
  } catch (const std::exception&) {
    global_router_cfg.reset();
    project_router_cfg.reset();
  }
  data["router_config"] = {
      {"global",
       {
           {"router_model", global_router_cfg.has_value() ? nlohmann::json(global_router_cfg->router_model)
                                                          : nlohmann::json(nullptr)},
           {"updated_at", global_router_cfg.has_value() ? nlohmann::json(global_router_cfg->updated_at)
                                                        : nlohmann::json(nullptr)},
       }},
      {"project",
       project_id.empty()
           ? nlohmann::json(nullptr)
           : nlohmann::json{
                 {"project_id", project_id},
                 {"router_model", project_router_cfg.has_value()
                                      ? nlohmann::json(project_router_cfg->router_model)
                                      : nlohmann::json(nullptr)},
                 {"updated_at", project_router_cfg.has_value()
                                    ? nlohmann::json(project_router_cfg->updated_at)
                                    : nlohmann::json(nullptr)},
             }},
  };
  std::string router_effective_scope = "auto";
  nlohmann::json router_effective_model = nullptr;
  if (project_router_cfg.has_value()) {
    router_effective_scope = "project";
    router_effective_model = project_router_cfg->router_model;
  } else if (global_router_cfg.has_value()) {
    router_effective_scope = "global";
    router_effective_model = global_router_cfg->router_model;
  }
  data["router_config"]["effective"] = {
      {"scope", router_effective_scope},
      {"router_model", router_effective_model},
  };
  const auto machine_caste = support::detect_machine_caste();
  const auto model_meta = support::load_local_model_meta();
  if (machine_caste.has_value()) {
    data["caste"] = {
        {"name", machine_caste->name},
        {"reason",
         machine_caste->reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(machine_caste->reason)},
    };
  } else {
    data["caste"] = nullptr;
  }
  if (!runner) {
    data["runner_available"] = false;
    data["error"] = "Local model runner not configured.";
    data["last_checked"] = support::now_epoch_seconds();
    data["models"] = nlohmann::json::array();
    if (machine_caste.has_value()) {
      const auto recommendations = support::build_caste_recommendations({}, model_meta, machine_caste->name);
      nlohmann::json all = nlohmann::json::array();
      for (const auto& item : recommendations) {
        all.push_back(item);
      }
      data["recommended_models"] = all;
      data["recommended_install"] = all;
    } else {
      data["recommended_models"] = nlohmann::json::array();
      data["recommended_install"] = nlohmann::json::array();
    }
  } else {
    const auto status = runner->status();
    data["runner_available"] = status.available;
    data["spawn_attempted"] = status.spawn_attempted;
    data["last_checked"] = status.last_checked;
    data["version"] = status.version;
    data["error"] = status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
    nlohmann::json models = nlohmann::json::array();
    for (const auto& model : status.models) {
      models.push_back({
          {"name", model.name},
          {"digest", model.digest},
          {"size", model.size},
          {"modified_at", model.modified_at},
      });
    }
    data["models"] = models;

    if (machine_caste.has_value()) {
      const auto recommendations =
          support::build_caste_recommendations(status.models, model_meta, machine_caste->name);
      nlohmann::json all = nlohmann::json::array();
      nlohmann::json to_install = nlohmann::json::array();
      for (const auto& item : recommendations) {
        all.push_back(item);
        if (!item.value("installed", false)) {
          to_install.push_back(item);
        }
      }
      data["recommended_models"] = all;
      data["recommended_install"] = to_install;
    } else {
      data["recommended_models"] = nlohmann::json::array();
      data["recommended_install"] = nlohmann::json::array();
    }
  }
  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = data;
  res = support::json_response(http::status::ok, payload);
  return true;
}

} // namespace holder::api::routes::ai::status
