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
    const std::function<std::string(const std::string&)>& param_get
) {
  RunnerRouteDispatchResult out{};
  const std::string runner_id = requested_runner_id(param_get);
  auto* runner = runner_registry ? runner_registry->get_client(runner_id) : nullptr;

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
    res = support::error_response(
        http::status::not_found,
        "not_found",
        "Pull job not found."
    ); // LCOV_EXCL_LINE
    return out; // LCOV_EXCL_LINE
  }

  out.streamed = true;
  http::response<http::empty_body> sse{http::status::ok, 11};
  sse.set(http::field::content_type, "text/event-stream");
  sse.set(http::field::cache_control, "no-cache");
  sse.set(http::field::connection, "keep-alive");
  sse.keep_alive(true);
  http::serializer<false, http::empty_body> sr{sse};
  boost::system::error_code write_ec;
  http::write_header(socket, sr, write_ec);
  if (write_ec) {
    return out; // LCOV_EXCL_LINE
  }

  auto send_event = [&](const std::string& name, const nlohmann::json& data) -> bool {
    std::string payload = "event: " + name + "\n";
    payload += "data: " + data.dump() + "\n\n";
    boost::system::error_code send_ec;
    boost::asio::write(socket, boost::asio::buffer(payload), send_ec);
    return !send_ec;
  };

  std::string last_status;
  long long last_completed = -1;
  long long last_total = -1;
  for (;;) {
    const auto job = runner->get_pull(job_id);
    if (!job.has_value()) {
      send_event(
          "failed",
          nlohmann::json{{"runner_id", runner_id}, {"error", "Pull job not found."}}
      );
      break;
    }

    const bool changed = job->status != last_status || job->progress.completed != last_completed ||
                         job->progress.total != last_total; // LCOV_EXCL_LINE
    if (changed) {
      const auto data = pull_job_to_json(job.value(), runner_id);
      if (!send_event("progress", data)) {
        break; // LCOV_EXCL_LINE
      }

      last_status = job->status;
      last_completed = job->progress.completed;
      last_total = job->progress.total;

      if (job->status == "completed") {
        send_event("completed", data);
        break;
      }
      if (job->status == "failed") { // LCOV_EXCL_LINE
        send_event("failed", data); // LCOV_EXCL_LINE
        break; // LCOV_EXCL_LINE
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // LCOV_EXCL_LINE
  }

  return out;
}

} // namespace holder::api::routes::ai::runner
