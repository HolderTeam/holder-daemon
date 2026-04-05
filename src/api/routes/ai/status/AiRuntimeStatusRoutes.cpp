#include "api/routes/ai/status/AiRuntimeStatusRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/ProviderUtils.h"
#include "api/support/Time.h"
#include "ai/AiProviderCredentialRepo.h"
#include "llm/LocalModelRunner.h"
#include "llm/RunnerModelRef.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <stdexcept>
#include <string>

namespace holder::api::routes::ai::status {
namespace {

namespace http = boost::beast::http;

std::string requested_runner_id(const http::request<http::string_body>& req,
                                const std::function<std::string(const std::string&)>& param_get) {
  const auto query_runner_id = param_get("runner_id");
  if (!query_runner_id.empty()) {
    return query_runner_id;
  }
  if (req.method() == http::verb::post && !req.body().empty()) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (body.contains("runner_id") && !body.at("runner_id").is_null()) {
        const auto value = body.at("runner_id").get<std::string>();
        if (!value.empty()) {
          return value;
        }
      }
    } catch (const std::exception&) {
    }
  }
  return holder::llm::RunnerRegistry::kAutoLocalRunnerId;
}

long long count_started_runs(holder::platform::Db& db) {
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

std::string preview_for_output(const std::string& stored) {
  return stored.find('*') != std::string::npos ? stored : support::mask_api_key(stored);
}

nlohmann::json runtime_model_to_json(const holder::llm::LocalModel& model, const std::string& runner_id) {
  return {
      {"runner_id", runner_id},
      {"name", model.name},
      {"model_ref", holder::llm::make_runner_model_ref(runner_id, model.name)},
      {"digest", model.digest},
      {"size", model.size},
      {"modified_at", model.modified_at},
  };
}

nlohmann::json runtime_pull_to_json(const holder::llm::RunnerPullJob& job, const std::string& runner_id) {
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
      {"active",
       (job.status == "queued" || job.status == "downloading" || job.status == "verifying")},
  };
}

nlohmann::json runner_runtime_to_json(const holder::model::AiRunner& runner,
                                      holder::llm::RunnerClient* client) {
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
  runtime["version"] = status.version.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.version);
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

nlohmann::json runner_to_json(const holder::model::AiRunner& runner,
                              holder::llm::RunnerRegistry* runner_registry) {
  return {
      {"runner_id", runner.runner_id},
      {"name", runner.name},
      {"kind", runner.kind},
      {"base_url", runner.base_url.has_value() ? nlohmann::json(runner.base_url.value()) : nlohmann::json(nullptr)},
      {"source", runner.source},
      {"enabled", runner.enabled},
      {"created_at", runner.created_at},
      {"updated_at", runner.updated_at},
      {"runtime",
       runner_runtime_to_json(
           runner, runner_registry ? runner_registry->get_client(runner.runner_id) : nullptr)},
  };
}

long long active_pull_jobs_from_runtime(const nlohmann::json& runtime) {
  long long active_pull_jobs = 0;
  if (!runtime.contains("pulls") || !runtime.at("pulls").is_array()) {
    return active_pull_jobs;
  }
  for (const auto& pull : runtime.at("pulls")) {
    if (pull.value("active", false)) {
      ++active_pull_jobs;
    }
  }
  return active_pull_jobs;
}

} // namespace

bool handle_ai_runtime_status_routes(const std::string& path,
                                     const http::request<http::string_body>& req,
                                     http::response<http::string_body>& res,
                                     holder::platform::Db& db,
                                     holder::llm::RunnerRegistry* runner_registry,
                                     const std::function<std::string(const std::string&)>& param_get) {
  const std::string runner_id = requested_runner_id(req, param_get);
  auto* runner = runner_registry ? runner_registry->get_client(runner_id) : nullptr;
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

    nlohmann::json runners = nlohmann::json::array();
    long long active_pull_jobs = 0;
    if (runner_registry != nullptr) {
      for (const auto& runner_record : runner_registry->list_runners()) {
        auto item = runner_to_json(runner_record, runner_registry);
        active_pull_jobs += active_pull_jobs_from_runtime(item["runtime"]);
        runners.push_back(std::move(item));
      }
    }
    data["runners"] = runners;

    if (!runner) {
      data["runner_id"] = runner_id;
      data["runner_available"] = false;
      data["runner_error"] = "Runner not configured.";
      data["runner_last_checked"] = support::now_epoch_seconds();
      data["active_pull_jobs"] = active_pull_jobs;
      data["pulls"] = nlohmann::json::array();
    } else {
      const auto status = runner->status();
      data["runner_id"] = runner_id;
      data["runner_available"] = status.available;
      data["runner_error"] =
          status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
      data["runner_last_checked"] = status.last_checked;
      data["runner_version"] =
          status.version.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.version);

      nlohmann::json pulls = nlohmann::json::array();
      for (const auto& job : runner->list_pulls()) {
        pulls.push_back(runtime_pull_to_json(job, runner_id));
      }
      data["active_pull_jobs"] = active_pull_jobs;
      data["pulls"] = pulls;
    }

    holder::ai::AiProviderCredentialRepo credential_repo(db);
    nlohmann::json cloud = nlohmann::json::array();
    for (const auto& credential : credential_repo.list()) {
      cloud.push_back({
          {"provider", credential.provider},
          {"configured", true},
          {"api_key_preview", preview_for_output(credential.api_key_preview)},
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
      res = support::error_response(http::status::not_found, "not_found", "Runner not configured.");
      return true;
    }

    const auto status = runner->retry();
    nlohmann::json data;
    data["runner_id"] = runner_id;
    data["runner_available"] = status.available;
    data["spawn_attempted"] = status.spawn_attempted;
    data["last_checked"] = status.last_checked;
    data["version"] = status.version;
    data["error"] = status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
    nlohmann::json models = nlohmann::json::array();
    for (const auto& model : status.models) {
      models.push_back({
          {"runner_id", runner_id},
          {"name", model.name},
          {"model_ref", holder::llm::make_runner_model_ref(runner_id, model.name)},
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

  return false;
}

} // namespace holder::api::routes::ai::status
