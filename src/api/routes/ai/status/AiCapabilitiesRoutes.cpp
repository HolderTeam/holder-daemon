#include "api/routes/ai/status/AiCapabilitiesRoutes.h"

#include "ai/AiLocalModelConfigRepo.h"
#include "api/support/HttpResponses.h"
#include "api/support/LocalModelRouting.h"
#include "api/support/Time.h"
#include "llm/LocalModelRunner.h"
#include "llm/RunnerModelRef.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes::ai::status {
namespace {

namespace http = boost::beast::http;

nlohmann::json runtime_model_to_json(
    const holder::llm::LocalModel& model,
    const std::string& runner_id
) {
  return {
      {"runner_id", runner_id},
      {"name", model.name},
      {"model_ref", holder::llm::make_runner_model_ref(runner_id, model.name)},
      {"digest", model.digest},
      {"size", model.size},
      {"modified_at", model.modified_at},
  };
}

nlohmann::json runtime_pull_to_json(
    const holder::llm::RunnerPullJob& job,
    const std::string& runner_id
) {
  return {
      {"job_id", job.job_id},
      {"runner_id", runner_id},
      {"model", job.model},
      {"model_ref", holder::llm::make_runner_model_ref(runner_id, job.model)},
      {"status", job.status},
      {"updated_at", job.updated_at},
      {"error", job.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job.error)},
      {"progress",
       {{"completed", job.progress.completed},
        {"total", job.progress.total},
        {"percent", job.progress.percent},
        {"stage", job.progress.stage}}},
  };
}

nlohmann::json runner_runtime_to_json(
    const holder::model::AiRunner& runner,
    holder::llm::RunnerClient* client
) {
  nlohmann::json runtime;
  runtime["configured"] = client != nullptr;
  if (client == nullptr) {
    runtime["available"] = false;
    runtime["spawn_attempted"] = false;
    runtime["last_checked"] = 0;
    runtime["version"] = nullptr;
    runtime["error"] = "runner_not_configured";
    runtime["models"] = nlohmann::json::array();
    runtime["pulls"] = nlohmann::json::array();
    return runtime;
  }

  const auto status = client->status();
  runtime["available"] = status.available;
  runtime["spawn_attempted"] = status.spawn_attempted;
  runtime["last_checked"] = status.last_checked;
  runtime["version"] = status.version.empty() ? nlohmann::json(nullptr)
                                              : nlohmann::json(status.version);
  runtime["error"] = status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
  runtime["models"] = nlohmann::json::array();
  for (const auto& model : status.models) {
    runtime["models"].push_back(runtime_model_to_json(model, runner.runner_id));
  }
  runtime["pulls"] = nlohmann::json::array();
  for (const auto& pull : client->list_pulls()) {
    runtime["pulls"].push_back(runtime_pull_to_json(pull, runner.runner_id));
  }
  return runtime;
}

nlohmann::json runner_to_json(
    const holder::model::AiRunner& runner,
    holder::llm::RunnerRegistry* runner_registry
) {
  return {
      {"runner_id", runner.runner_id},
      {"name", runner.name},
      {"kind", runner.kind},
      {"base_url",
       runner.base_url.has_value() ? nlohmann::json(runner.base_url.value())
                                   : nlohmann::json(nullptr)},
      {"source", runner.source},
      {"enabled", runner.enabled},
      {"created_at", runner.created_at},
      {"updated_at", runner.updated_at},
      {"runtime",
       runner_runtime_to_json(
           runner,
           runner_registry ? runner_registry->get_client(runner.runner_id) : nullptr
       )},
  };
}

} // namespace

bool handle_ai_capabilities_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string(const std::string&)>& param_get
) {
  if (path != "/ai/capabilities" || req.method() != http::verb::get) {
    return false;
  }
  const auto runner_id = param_get("runner_id").empty()
                             ? std::string(holder::llm::RunnerRegistry::kAutoLocalRunnerId)
                             : param_get("runner_id");
  auto* runner = runner_registry ? runner_registry->get_client(runner_id) : nullptr;

  nlohmann::json data;
  std::optional<holder::model::AiLocalModelConfig> local_model_cfg;
  try {
    holder::ai::AiLocalModelConfigRepo local_model_cfg_repo(db);
    local_model_cfg = local_model_cfg_repo.get();
  } catch (const std::exception&) {
    local_model_cfg.reset();
  }
  data["local_model_config"] = {
      {"fast_model",
       local_model_cfg.has_value() && local_model_cfg->fast_model.has_value()
           ? nlohmann::json(local_model_cfg->fast_model.value())
           : nlohmann::json(nullptr)},
      {"strong_model",
       local_model_cfg.has_value() && local_model_cfg->strong_model.has_value()
           ? nlohmann::json(local_model_cfg->strong_model.value())
           : nlohmann::json(nullptr)},
      {"deep_model",
       local_model_cfg.has_value() && local_model_cfg->deep_model.has_value()
           ? nlohmann::json(local_model_cfg->deep_model.value())
           : nlohmann::json(nullptr)},
      {"updated_at",
       local_model_cfg.has_value() ? nlohmann::json(local_model_cfg->updated_at)
                                   : nlohmann::json(nullptr)},
  };
  const auto machine_caste = support::detect_machine_caste();
  const auto model_meta = support::load_local_model_meta();
  if (machine_caste.has_value()) {
    data["caste"] = {
        {"name", machine_caste->name},
        {"reason",
         machine_caste->reason.empty() ? nlohmann::json(nullptr)
                                       : nlohmann::json(machine_caste->reason)},
    };
  } else {
    data["caste"] = nullptr; // LCOV_EXCL_LINE
  }
  nlohmann::json runners = nlohmann::json::array();
  if (runner_registry != nullptr) {
    for (const auto& runner_record : runner_registry->list_runners()) {
      runners.push_back(runner_to_json(runner_record, runner_registry));
    }
  }
  data["runners"] = runners;
  if (!runner) {
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
      data["recommended_models"] = nlohmann::json::array(); // LCOV_EXCL_LINE
      data["recommended_install"] = nlohmann::json::array(); // LCOV_EXCL_LINE
    }
  } else {
    const auto status = runner->status();
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
      data["recommended_models"] = nlohmann::json::array(); // LCOV_EXCL_LINE
      data["recommended_install"] = nlohmann::json::array(); // LCOV_EXCL_LINE
    }
  }
  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = data;
  res = support::json_response(http::status::ok, payload);
  return true;
}

} // namespace holder::api::routes::ai::status
