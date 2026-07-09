#include "api/routes/ai/runner/AiRunnerPullRoutes.h"

#include "api/support/HttpResponses.h"
#include "llm/LocalModelRunner.h"
#include "llm/RunnerModelRef.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string>

namespace holder::api::routes::ai::runner {
namespace {

namespace http = boost::beast::http;

nlohmann::json pull_job_to_json(
    const holder::llm::RunnerPullJob& job,
    const std::string& runner_id
) {
  nlohmann::json data;
  data["job_id"] = job.job_id;
  data["runner_id"] = runner_id;
  data["model"] = job.model;
  data["model_ref"] = holder::llm::make_runner_model_ref(runner_id, job.model);
  data["status"] = job.status;
  data["updated_at"] = job.updated_at;
  data["error"] = job.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job.error);
  data["progress"] = {
      {"completed", job.progress.completed},
      {"total", job.progress.total},
      {"percent", job.progress.percent},
      {"stage", job.progress.stage},
  };
  return data;
}

std::string requested_runner_id(
    const http::request<http::string_body>& req,
    const std::function<std::string(const std::string&)>& param_get
) {
  auto query_runner_id = param_get("runner_id");
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

} // namespace

RunnerRouteDispatchResult handle_ai_runner_pull_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string(const std::string&)>& param_get
) {
  RunnerRouteDispatchResult out{};
  const std::string runner_id = requested_runner_id(req, param_get);
  auto* runner = runner_registry ? runner_registry->get_client(runner_id) : nullptr;

  if (path == "/ai/runner/pull" && req.method() == http::verb::post) {
    out.handled = true;
    if (!runner) {
      res = support::error_response(http::status::not_found, "not_found", "Runner not configured.");
      return out;
    }
    try {
      const auto runner_status = runner->status();
      if (!runner_status.available) {
        res = support::error_response(
            http::status::service_unavailable,
            "runner_unavailable",
            "Local model runner unavailable."
        );
      } else {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("model")) {
          res = support::error_response(http::status::bad_request, "bad_request", "Missing model.");
        } else {
          const std::string model = body.at("model").get<std::string>();
          spdlog::info("AI runner pull requested runner_id=" + runner_id + " model=" + model);
          auto job = runner->start_pull(model);
          if (job.status == "failed") {
            res = support::error_response(
                http::status::bad_request,
                "bad_request",
                job.error.empty() ? "Pull failed." : job.error
            );
          } else {
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {
                {"job_id", job.job_id},
                {"runner_id", runner_id},
                {"model", job.model},
                {"model_ref", holder::llm::make_runner_model_ref(runner_id, job.model)},
                {"status", job.status},
            };
            res = support::json_response(http::status::ok, payload);
          }
        }
      }
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return out;
  }

  if (path.rfind("/ai/runner/pull/", 0) == 0 && req.method() == http::verb::get) {
    out.handled = true;
    if (!runner) {
      res = support::error_response(http::status::not_found, "not_found", "Runner not configured.");
      return out;
    }
    const std::string job_id = path.substr(std::string("/ai/runner/pull/").size());
    if (job_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Pull job not found.");
      return out;
    }

    const auto job = runner->get_pull(job_id);
    if (!job.has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "Pull job not found.");
      return out;
    }

    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = pull_job_to_json(job.value(), runner_id);
    res = support::json_response(http::status::ok, payload);
    return out;
  }

  return out;
}

} // namespace holder::api::routes::ai::runner
