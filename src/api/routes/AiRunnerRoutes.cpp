#include "api/routes/AiRunnerRoutes.h"
#include "api/support/HttpResponses.h"

#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

} // namespace

RunnerRouteDispatchResult handle_ai_runner_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::llm::LocalModelRunner* runner) {
  RunnerRouteDispatchResult out{};

  if (path.rfind("/ai/runner/pull/", 0) == 0 &&
      path.size() > std::string("/ai/runner/pull/").size() + std::string("/events").size() &&
      path.compare(path.size() - std::string("/events").size(),
                   std::string("/events").size(),
                   "/events") == 0 &&
      req.method() == http::verb::get) {
    out.handled = true;
    if (!runner) {
      res = support::error_response(http::status::not_implemented,
                           "not_implemented",
                           "Local model runner not configured.");
      return out;
    }
    const std::string prefix = "/ai/runner/pull/";
    const std::string suffix = "/events";
    const std::string job_id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (job_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Pull job not found.");
      return out;
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
      return out;
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
        send_event("failed", nlohmann::json{{"error", "Pull job not found."}});
        break;
      }

      const bool changed = job->status != last_status || job->progress.completed != last_completed ||
                           job->progress.total != last_total;
      if (changed) {
        nlohmann::json data;
        data["job_id"] = job->job_id;
        data["model"] = job->model;
        data["status"] = job->status;
        data["updated_at"] = job->updated_at;
        data["error"] = job->error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job->error);
        data["progress"] = {
            {"completed", job->progress.completed},
            {"total", job->progress.total},
            {"percent", job->progress.percent},
            {"stage", job->progress.stage},
        };

        if (!send_event("progress", data)) {
          break;
        }

        last_status = job->status;
        last_completed = job->progress.completed;
        last_total = job->progress.total;

        if (job->status == "completed") {
          send_event("completed", data);
          break;
        }
        if (job->status == "failed") {
          send_event("failed", data);
          break;
        }
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return out;
  }

  if (path == "/ai/runner/pull" && req.method() == http::verb::post) {
    out.handled = true;
    if (!runner) {
      res = support::error_response(http::status::not_implemented,
                           "not_implemented",
                           "Local model runner not configured.");
      return out;
    }
    try {
      const auto runner_status = runner->status();
      if (!runner_status.available) {
        res = support::error_response(http::status::service_unavailable,
                             "runner_unavailable",
                             "Local model runner unavailable.");
      } else {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("model")) {
          res = support::error_response(http::status::bad_request, "bad_request", "Missing model.");
        } else {
          const std::string model = body.at("model").get<std::string>();
          auto job = runner->start_pull(model);
          if (job.status == "failed") {
            res = support::error_response(http::status::bad_request,
                                 "bad_request",
                                 job.error.empty() ? "Pull failed." : job.error);
          } else {
            nlohmann::json data;
            data["job_id"] = job.job_id;
            data["model"] = job.model;
            data["status"] = job.status;
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = data;
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
      res = support::error_response(http::status::not_implemented,
                           "not_implemented",
                           "Local model runner not configured.");
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

    nlohmann::json data;
    data["job_id"] = job->job_id;
    data["model"] = job->model;
    data["status"] = job->status;
    data["updated_at"] = job->updated_at;
    data["error"] = job->error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job->error);
    nlohmann::json progress;
    progress["completed"] = job->progress.completed;
    progress["total"] = job->progress.total;
    progress["percent"] = job->progress.percent;
    progress["stage"] = job->progress.stage;
    data["progress"] = progress;
    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = data;
    res = support::json_response(http::status::ok, payload);
    return out;
  }

  return out;
}

} // namespace holder::api::routes
