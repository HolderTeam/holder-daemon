#include "api/routes/ai/runner/AiRunnerPullEventRoutes.h"

#include "api/support/HttpResponses.h"
#include "llm/LocalModelRunner.h"
#include "llm/RunnerModelRef.h"

#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

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

std::string requested_runner_id(const std::function<std::string(const std::string&)>& param_get) {
  const auto query_runner_id = param_get("runner_id");
  return query_runner_id.empty() ? std::string(holder::llm::RunnerRegistry::kAutoLocalRunnerId)
                                 : query_runner_id;
}

} // namespace

RunnerRouteDispatchResult handle_ai_runner_pull_event_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string(const std::string&)>& param_get,
    std::shared_ptr<holder::api::support::SseRegistry> streams
) {
  RunnerRouteDispatchResult out{};
  const std::string runner_id = requested_runner_id(param_get);
  auto runner = runner_registry ? runner_registry->share_client(runner_id) : nullptr;

  if (path.rfind("/ai/runner/pull/", 0) != 0 ||
      path.size() <= std::string("/ai/runner/pull/").size() + std::string("/events").size() ||
      path.compare(
          path.size() - std::string("/events").size(),
          std::string("/events").size(),
          "/events"
      ) != 0 ||
      req.method() != http::verb::get) {
    return out;
  }

  out.handled = true;
  if (!runner) {
    res = support::error_response(http::status::not_found, "not_found", "Runner not configured.");
    return out;
  }

  const std::string prefix = "/ai/runner/pull/";
  const std::string suffix = "/events";
  const std::string job_id =
      path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (job_id.empty()) {
    // LCOV_EXCL_START
    res = support::error_response(http::status::not_found, "not_found", "Pull job not found.");
    return out;
    // LCOV_EXCL_STOP
  }

  const std::string last_event_id(req["Last-Event-ID"]);
  if (!last_event_id.empty() && !support::EventJournal::valid_cursor(last_event_id)) {
    res =
        support::error_response(http::status::bad_request, "bad_request", "Invalid Last-Event-ID.");
    return out;
  }
  // Pull progress is a replaceable status snapshot, not a token/delta history.
  // Each connection has its own incarnation; reconnect with an old cursor asks
  // the caller to refresh the authoritative job-status resource.
  auto journal = std::make_shared<support::EventJournal>();
  auto source =
      [runner, runner_id, job_id, journal, cursor = last_event_id, previous = std::string()](
      ) mutable {
        const auto job = runner->get_pull(job_id);
        if (!job) {
          if (previous.empty()) {
            journal->append(
                "failed",
                {{"runner_id", runner_id}, {"job_id", job_id}, {"error", "Pull job not found."}},
                true
            );
            previous = "missing";
          }
        } else {
          const auto data = pull_job_to_json(*job, runner_id);
          const auto current = data.dump();
          if (current != previous) {
            journal->append("progress", data);
            if (job->status == "completed" || job->status == "failed")
              journal->append(job->status == "completed" ? "completed" : "failed", data, true);
            previous = current;
          }
        }
        auto batch = journal->read(cursor);
        cursor = batch.cursor;
        return batch;
      };
  auto stream = support::SseStream::start(socket, std::move(streams), std::move(source));
  if (!stream) {
    res = support::error_response(
        http::status::service_unavailable,
        "server_busy",
        "Too many event streams."
    );
    return out;
  }
  out.streamed = true;

  return out;
}

} // namespace holder::api::routes::ai::runner
