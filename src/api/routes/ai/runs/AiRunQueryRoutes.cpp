#include "api/routes/ai/runs/AiRunQueryRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/RunEventStore.h"
#include "api/support/Time.h"
#include "ai/AiRunRepo.h"

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
  item["run_id"] = run.run_id;
  item["project_id"] =
      run.project_id.has_value() ? nlohmann::json(run.project_id.value()) : nlohmann::json(nullptr);
  item["thread_id"] =
      run.thread_id.has_value() ? nlohmann::json(run.thread_id.value()) : nlohmann::json(nullptr);
  item["message_id"] =
      run.message_id.has_value() ? nlohmann::json(run.message_id.value()) : nlohmann::json(nullptr);
  item["mode"] = run.mode;
  item["prompt"] = run.prompt;
  item["context_json"] =
      run.context_json.has_value() ? nlohmann::json(run.context_json.value()) : nlohmann::json(nullptr);
  item["router_model"] =
      run.router_model.has_value() ? nlohmann::json(run.router_model.value()) : nlohmann::json(nullptr);
  item["ranked_json"] =
      run.ranked_json.has_value() ? nlohmann::json(run.ranked_json.value()) : nlohmann::json(nullptr);
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
  item["chosen_model"] =
      run.chosen_model.has_value() ? nlohmann::json(run.chosen_model.value()) : nlohmann::json(nullptr);
  item["status"] = run.status;
  item["error"] = run.error.has_value() ? nlohmann::json(run.error.value()) : nlohmann::json(nullptr);
  item["created_at"] = run.created_at;
  item["updated_at"] = run.updated_at;
  return item;
}

} // namespace

RouteDispatchResult handle_ai_runs_list_route(
    const std::function<std::string(const std::string&)>& param_get,
    http::response<http::string_body>& res,
    holder::store::Db& db) {
  RouteDispatchResult out{};
  out.handled = true;
  const std::string project_id = param_get("project_id");
  const std::string thread_id = param_get("thread_id");
  if (project_id.empty() && thread_id.empty()) {
    res = support::error_response(http::status::bad_request,
                                  "bad_request",
                                  "Missing project_id or thread_id.");
    return out;
  }
  try {
    holder::store::AiRunRepo repo(db);
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

RouteDispatchResult handle_ai_runs_events_route(const std::string& path,
                                                boost::asio::ip::tcp::socket& socket,
                                                http::response<http::string_body>& res,
                                                holder::store::Db& db) {
  RouteDispatchResult out{};
  out.handled = true;
  out.streamed = true;
  const std::string prefix = "/ai/runs/";
  const std::string suffix = "/events";
  const std::string run_id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (run_id.empty()) {
    out.streamed = false;
    res = support::error_response(http::status::not_found, "not_found", "Run not found.");
    return out;
  }

  std::optional<holder::model::AiRun> run_record;
  try {
    holder::store::AiRunRepo repo(db);
    run_record = repo.get(run_id);
  } catch (const std::exception&) {
    run_record = std::nullopt;
  }
  if (!run_record.has_value()) {
    out.streamed = false;
    res = support::error_response(http::status::not_found, "not_found", "Run not found.");
    return out;
  }

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

  auto write_sse = [&](const std::string& name, const nlohmann::json& data) -> bool {
    std::string payload = "event: " + name + "\n";
    payload += "data: " + data.dump() + "\n\n";
    boost::system::error_code send_ec;
    boost::asio::write(socket, boost::asio::buffer(payload), send_ec);
    return !send_ec;
  };

  size_t cursor = 0;
  const long long started = support::now_epoch_seconds();
  for (;;) {
    const auto stream = support::get_run_event_stream(run_id);
    if (stream.has_value()) {
      while (cursor < stream->events.size()) {
        if (!write_sse(stream->events[cursor].name, stream->events[cursor].data)) {
          return out;
        }
        ++cursor;
      }
      if (stream->finished) {
        return out;
      }
    } else if (run_record->status == "completed" || run_record->status == "failed") {
      nlohmann::json terminal;
      terminal["run_id"] = run_id;
      if (run_record->chosen_model.has_value()) {
        terminal["model"] = run_record->chosen_model.value();
      }
      if (run_record->error.has_value()) {
        terminal["error"] = run_record->error.value();
      }
      if (run_record->status == "completed") {
        write_sse("done", terminal);
      } else {
        write_sse("failed", terminal);
      }
      return out;
    }

    if (support::now_epoch_seconds() - started > 60) {
      nlohmann::json keepalive;
      keepalive["run_id"] = run_id;
      keepalive["status"] = "pending";
      write_sse("pending", keepalive);
      return out;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

RouteDispatchResult handle_ai_runs_get_route(const std::string& path,
                                             http::response<http::string_body>& res,
                                             holder::store::Db& db) {
  RouteDispatchResult out{};
  out.handled = true;
  const std::string run_id = path.substr(std::string("/ai/runs/").size());
  if (run_id.empty()) {
    res = support::error_response(http::status::not_found, "not_found", "Run not found.");
    return out;
  }
  try {
    holder::store::AiRunRepo repo(db);
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
