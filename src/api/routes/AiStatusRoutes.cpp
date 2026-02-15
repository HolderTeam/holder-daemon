#include "api/routes/AiStatusRoutes.h"
#include "api/support/HttpResponses.h"
#include "api/support/Time.h"

#include "api/support/LocalModelRouting.h"
#include "store/AiProviderCredentialRepo.h"
#include "store/AiRouterConfigRepo.h"
#include "store/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <optional>
#include <stdexcept>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

std::string mask_api_key(const std::string& api_key) {
  if (api_key.empty()) return {};
  if (api_key.size() <= 8) return "****";
  return api_key.substr(0, 4) + "..." + api_key.substr(api_key.size() - 2);
}

long long count_started_runs(holder::store::Db& db) {
  static constexpr const char* SQL = "SELECT COUNT(*) FROM ai_runs WHERE status = 'started';";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db.handle(), SQL, -1, &stmt, nullptr) != SQLITE_OK) {
    throw std::runtime_error("prepare count started runs failed");
  }
  long long out = 0;
  const int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    out = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return out;
}

} // namespace

bool handle_ai_status_routes(const std::string& path,
                             const http::request<http::string_body>& req,
                             http::response<http::string_body>& res,
                             holder::store::Db& db,
                             holder::llm::LocalModelRunner* runner,
                             const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/ai/capabilities" && req.method() == http::verb::get) {
    nlohmann::json data;
    const std::string project_id = param_get("project_id");
    std::optional<holder::model::AiRouterConfig> global_router_cfg;
    std::optional<holder::model::AiRouterConfig> project_router_cfg;
    try {
      holder::store::AiRouterConfigRepo router_cfg_repo(db);
      global_router_cfg = router_cfg_repo.get_global();
      project_router_cfg =
          project_id.empty() ? std::optional<holder::model::AiRouterConfig>{}
                             : router_cfg_repo.get_for_project(project_id);
    } catch (const std::exception&) {
      global_router_cfg.reset();
      project_router_cfg.reset();
    }
    data["router_config"] = {
        {"global",
         {
             {"router_model",
              global_router_cfg.has_value() ? nlohmann::json(global_router_cfg->router_model)
                                            : nlohmann::json(nullptr)},
             {"updated_at",
              global_router_cfg.has_value() ? nlohmann::json(global_router_cfg->updated_at)
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
          {"reason", machine_caste->reason.empty() ? nlohmann::json(nullptr)
                                                   : nlohmann::json(machine_caste->reason)},
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
        const auto recommendations =
            support::build_caste_recommendations({}, model_meta, machine_caste->name);
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

  if (path == "/ai/status" && req.method() == http::verb::get) {
    nlohmann::json data;
    data["checked_at"] = support::now_epoch_seconds();

    long long active_runs = 0;
    try {
      active_runs = count_started_runs(db);
    } catch (const std::exception&) {
      active_runs = 0;
    }
    data["active_runs"] = active_runs;

    if (!runner) {
      data["runner_available"] = false;
      data["runner_error"] = "Local model runner not configured.";
      data["runner_last_checked"] = support::now_epoch_seconds();
      data["active_pull_jobs"] = 0;
      data["pulls"] = nlohmann::json::array();
    } else {
      const auto status = runner->status();
      data["runner_available"] = status.available;
      data["runner_error"] =
          status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
      data["runner_last_checked"] = status.last_checked;
      data["runner_version"] =
          status.version.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.version);

      nlohmann::json pulls = nlohmann::json::array();
      long long active_pull_jobs = 0;
      for (const auto& job : runner->list_pulls()) {
        nlohmann::json item;
        item["job_id"] = job.job_id;
        item["model"] = job.model;
        item["status"] = job.status;
        item["updated_at"] = job.updated_at;
        item["error"] = job.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job.error);
        item["progress"] = {
            {"completed", job.progress.completed},
            {"total", job.progress.total},
            {"percent", job.progress.percent},
            {"stage", job.progress.stage},
        };
        const bool active =
            (job.status == "queued" || job.status == "downloading" || job.status == "verifying");
        item["active"] = active;
        if (active) ++active_pull_jobs;
        pulls.push_back(std::move(item));
      }
      data["active_pull_jobs"] = active_pull_jobs;
      data["pulls"] = pulls;
    }

    holder::store::AiProviderCredentialRepo credential_repo(db);
    nlohmann::json cloud = nlohmann::json::array();
    for (const auto& credential : credential_repo.list()) {
      cloud.push_back({
          {"provider", credential.provider},
          {"configured", true},
          {"api_key_preview", mask_api_key(credential.api_key)},
          {"created_at", credential.created_at},
          {"updated_at", credential.updated_at},
      });
    }
    data["cloud"] = cloud;
    data["cloud_configured_providers"] = static_cast<long long>(cloud.size());

    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = data;
    res = support::json_response(http::status::ok, payload);
    return true;
  }

  if (path == "/ai/runner/retry" && req.method() == http::verb::post) {
    if (!runner) {
      res = support::error_response(http::status::not_implemented,
                           "not_implemented",
                           "Local model runner not configured.");
      return true;
    }

    const auto status = runner->retry();
    nlohmann::json data;
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
    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = data;
    res = support::json_response(http::status::ok, payload);
    return true;
  }

  if (path == "/ai/router/config" && req.method() == http::verb::get) {
    try {
      const std::string project_id = param_get("project_id");
      holder::store::AiRouterConfigRepo repo(db);
      const auto global_cfg = repo.get_global();
      const auto project_cfg =
          project_id.empty() ? std::optional<holder::model::AiRouterConfig>{}
                             : repo.get_for_project(project_id);

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

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = data;
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
        res = support::error_response(http::status::bad_request,
                             "bad_request",
                             "scope must be global or project.");
        return true;
      }

      bool valid = true;
      std::string project_id;
      if (scope == "project") {
        if (!body.contains("project_id") || body.at("project_id").is_null()) {
          res = support::error_response(http::status::bad_request,
                               "bad_request",
                               "project_id required for project scope.");
          valid = false;
        }
        if (valid) {
          project_id = body.at("project_id").get<std::string>();
        }
        if (valid && project_id.empty()) {
          res = support::error_response(http::status::bad_request,
                               "bad_request",
                               "project_id required for project scope.");
          valid = false;
        }
        if (valid) {
          holder::store::ProjectRepo projects(db);
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

        holder::store::AiRouterConfigRepo repo(db);
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

        nlohmann::json data;
        data["global"] = {
            {"router_model", global_cfg.has_value() ? nlohmann::json(global_cfg->router_model)
                                                    : nlohmann::json(nullptr)},
            {"updated_at", global_cfg.has_value() ? nlohmann::json(global_cfg->updated_at)
                                                  : nlohmann::json(nullptr)},
        };
        if (scope == "project" && !project_id.empty()) {
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

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = support::json_response(http::status::ok, payload);
      }
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
