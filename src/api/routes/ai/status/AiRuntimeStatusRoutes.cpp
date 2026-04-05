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

} // namespace

bool handle_ai_runtime_status_routes(const std::string& path,
                                     const http::request<http::string_body>& req,
                                     http::response<http::string_body>& res,
                                     holder::platform::Db& db,
                                     holder::llm::RunnerRegistry* runner_registry) {
  auto* runner = runner_registry ? runner_registry->get_auto_local_runner() : nullptr;
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
      data["runner_id"] = holder::llm::RunnerRegistry::kAutoLocalRunnerId;
      data["runner_available"] = false;
      data["runner_error"] = "Local model runner not configured.";
      data["runner_last_checked"] = support::now_epoch_seconds();
      data["active_pull_jobs"] = 0;
      data["pulls"] = nlohmann::json::array();
    } else {
      const auto status = runner->status();
      data["runner_id"] = holder::llm::RunnerRegistry::kAutoLocalRunnerId;
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
        item["runner_id"] = holder::llm::RunnerRegistry::kAutoLocalRunnerId;
        item["model"] = job.model;
        item["model_ref"] = holder::llm::make_runner_model_ref(
            holder::llm::RunnerRegistry::kAutoLocalRunnerId, job.model);
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
      res = support::error_response(http::status::not_implemented,
                                    "not_implemented",
                                    "Local model runner not configured.");
      return true;
    }

    const auto status = runner->retry();
    nlohmann::json data;
    data["runner_id"] = holder::llm::RunnerRegistry::kAutoLocalRunnerId;
    data["runner_available"] = status.available;
    data["spawn_attempted"] = status.spawn_attempted;
    data["last_checked"] = status.last_checked;
    data["version"] = status.version;
    data["error"] = status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
    nlohmann::json models = nlohmann::json::array();
    for (const auto& model : status.models) {
      models.push_back({
          {"runner_id", holder::llm::RunnerRegistry::kAutoLocalRunnerId},
          {"name", model.name},
          {"model_ref",
           holder::llm::make_runner_model_ref(holder::llm::RunnerRegistry::kAutoLocalRunnerId,
                                              model.name)},
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
