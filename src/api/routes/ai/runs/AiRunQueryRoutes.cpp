#include "api/routes/ai/runs/AiRunQueryRoutes.h"

#include "ai/AiRunRepo.h"
#include "api/support/HttpResponses.h"
#include "api/support/RunEventStore.h"
#include "api/support/Time.h"
#include "llm/RunnerModelRef.h"

#include <boost/asio/write.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace holder::api::routes::ai::runs {
namespace {

namespace http = boost::beast::http;

nlohmann::json parse_json_or_null(const std::optional<std::string>& raw) {
  if (!raw.has_value() || raw->empty()) return nullptr;
  try {
    return nlohmann::json::parse(raw.value());
  } catch (const std::exception&) {
    return nullptr;
  }
}

nlohmann::json ai_run_to_json(const holder::model::AiRun& run) {
  nlohmann::json item;
  const auto router_runner_id = holder::llm::runner_id_from_ref(run.router_model);
  const auto chosen_runner_id = holder::llm::runner_id_from_ref(run.chosen_model);
  item["run_id"] = run.run_id;
  item["project_id"] = run.project_id.has_value() ? nlohmann::json(run.project_id.value())
                                                  : nlohmann::json(nullptr);
  item["thread_id"] = run.thread_id.has_value() ? nlohmann::json(run.thread_id.value())
                                                : nlohmann::json(nullptr);
  item["message_id"] = run.message_id.has_value() ? nlohmann::json(run.message_id.value())
                                                  : nlohmann::json(nullptr);
  item["mode"] = run.mode;
  item["prompt"] = run.prompt;
  item["context_json"] = run.context_json.has_value() ? nlohmann::json(run.context_json.value())
                                                      : nlohmann::json(nullptr);
  item["router_model"] = run.router_model.has_value() ? nlohmann::json(run.router_model.value())
                                                      : nlohmann::json(nullptr);
  item["router_runner_id"] = router_runner_id.has_value() ? nlohmann::json(router_runner_id.value())
                                                          : nlohmann::json(nullptr);
  item["ranked_json"] = run.ranked_json.has_value() ? nlohmann::json(run.ranked_json.value())
                                                    : nlohmann::json(nullptr);
  item["policy_trace_json"] = run.policy_trace_json.has_value()
                                  ? nlohmann::json(run.policy_trace_json.value())
                                  : nlohmann::json(nullptr);
  nlohmann::json policy_trace = parse_json_or_null(run.policy_trace_json);
  if (policy_trace.is_null() && run.ranked_json.has_value()) {
    // Backward-compat for older rows where cloud policy traces were stored in ranked_json.
    const nlohmann::json ranked_maybe_obj = parse_json_or_null(run.ranked_json);
    if (ranked_maybe_obj.is_object()) {
      policy_trace = ranked_maybe_obj;
    }
  }
  item["policy_trace"] = policy_trace;
  item["chosen_model"] = run.chosen_model.has_value() ? nlohmann::json(run.chosen_model.value())
                                                      : nlohmann::json(nullptr);
  item["chosen_runner_id"] = chosen_runner_id.has_value() ? nlohmann::json(chosen_runner_id.value())
                                                          : nlohmann::json(nullptr);
  item["status"] = run.status;
  item["error"] = run.error.has_value() ? nlohmann::json(run.error.value())
                                        : nlohmann::json(nullptr);
  item["created_at"] = run.created_at;
  item["updated_at"] = run.updated_at;
  return item;
}

} // namespace

RouteDispatchResult handle_ai_runs_list_route(
    const std::function<std::string(const std::string&)>& param_get,
    http::response<http::string_body>& res,
    holder::platform::Db& db
) {
  RouteDispatchResult out{};
  out.handled = true;
  const std::string project_id = param_get("project_id");
  const std::string thread_id = param_get("thread_id");
  if (project_id.empty() && thread_id.empty()) {
    res = support::error_response(
        http::status::bad_request,
        "bad_request",
        "Missing project_id or thread_id."
    );
    return out;
  }
  try {
    holder::ai::AiRunRepo repo(db);
    std::vector<holder::model::AiRun> runs;
    if (!thread_id.empty()) {
      runs = repo.list_by_thread(thread_id);
    } else {
      runs = repo.list_by_project(project_id);
    }
    nlohmann::json data = nlohmann::json::array();
    for (const auto& run : runs) {
      data.push_back(ai_run_to_json(run));
    }
    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = data;
    res = support::json_response(http::status::ok, payload);
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  }
  return out;
}

RouteDispatchResult handle_ai_runs_events_route(
    const std::string& path,
    boost::asio::ip::tcp::socket& socket,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    const std::string& last_event_id,
    std::shared_ptr<support::SseRegistry> streams
) {
  RouteDispatchResult out{.handled = true};
  const std::string prefix = "/ai/runs/";
  const std::string suffix = "/events";
  const auto run_id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (!last_event_id.empty() && !support::EventJournal::valid_cursor(last_event_id)) {
    res =
        support::error_response(http::status::bad_request, "bad_request", "Invalid Last-Event-ID.");
    return out;
  }
  std::optional<holder::model::AiRun> run;
  try {
    holder::ai::AiRunRepo repo(db);
    run = repo.get(run_id);
  } catch (const std::exception&) {
  }
  if (!run || run_id.empty()) {
    res = support::error_response(http::status::not_found, "not_found", "Run not found.");
    return out;
  }
  // A completed run survives daemon restart even when its transient chunk history
  // does not. Return its terminal state on a fresh connection; a stale cursor
  // explicitly requests a refresh rather than silently replaying a new history.
  const auto stored = support::read_run_events(run_id, last_event_id);
  if (!stored && (run->status == "completed" || run->status == "failed")) {
    nlohmann::json terminal = {{"run_id", run_id}};
    if (run->chosen_model) {
      terminal["model_ref"] = *run->chosen_model;
      const auto parsed = holder::llm::parse_runner_model_ref(*run->chosen_model);
      terminal["model"] = parsed ? parsed->model_name : *run->chosen_model;
      if (parsed) terminal["runner_id"] = parsed->runner_id;
    }
    if (run->error) terminal["error"] = *run->error;
    support::append_run_event(
        run_id,
        run->status == "completed" ? "done" : "failed",
        terminal,
        true
    );
  }
  auto source = [run_id, cursor = last_event_id]() mutable {
    auto batch = support::read_run_events(run_id, cursor);
    if (!batch) {
      support::EventBatch result;
      result.resync_required = !cursor.empty();
      return result;
    }
    if (cursor.empty() && batch->truncated) batch->resync_required = true;
    cursor = batch->cursor;
    return *batch;
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

RouteDispatchResult handle_ai_runs_get_route(
    const std::string& path,
    http::response<http::string_body>& res,
    holder::platform::Db& db
) {
  RouteDispatchResult out{};
  out.handled = true;
  const std::string run_id = path.substr(std::string("/ai/runs/").size());
  if (run_id.empty()) {
    res = support::error_response(http::status::not_found, "not_found", "Run not found.");
    return out;
  }
  try {
    holder::ai::AiRunRepo repo(db);
    const auto run = repo.get(run_id);
    if (!run.has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "Run not found.");
    } else {
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = ai_run_to_json(run.value());
      res = support::json_response(http::status::ok, payload);
    }
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  }
  return out;
}

} // namespace holder::api::routes::ai::runs
