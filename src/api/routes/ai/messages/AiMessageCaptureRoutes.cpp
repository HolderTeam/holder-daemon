#include "api/routes/ai/messages/AiMessageCaptureRoutes.h"

#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "ai/AiThreadDurability.h"
#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "project/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes::ai::messages {
namespace {

namespace http = boost::beast::http;

} // namespace

bool handle_ai_message_capture_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    const std::function<std::string()>& uuid_v4
) {
  if (path != "/ai/messages/capture" || req.method() != http::verb::post) {
    return false;
  }

  try {
    const auto body = nlohmann::json::parse(req.body());
    if (!body.contains("project_id") || !body.contains("prompt") || !body.contains("response")) {
      res = support::error_response(
          http::status::bad_request,
          "bad_request",
          "Missing required fields."
      );
      return true;
    }

    const std::string project_id = body.at("project_id").get<std::string>();
    const std::string prompt = body.at("prompt").get<std::string>();
    const std::string response_text = body.at("response").get<std::string>();
    const std::string source = (body.contains("source") && !body.at("source").is_null())
                                   ? body.at("source").get<std::string>()
                                   : "manual_paste";

    std::optional<std::string> provider;
    if (body.contains("provider") && !body.at("provider").is_null()) {
      provider = body.at("provider").get<std::string>();
    }
    std::optional<std::string> model;
    if (body.contains("model") && !body.at("model").is_null()) {
      model = body.at("model").get<std::string>();
    }
    std::optional<std::string> thread_id;
    if (body.contains("thread_id") && !body.at("thread_id").is_null()) {
      thread_id = body.at("thread_id").get<std::string>();
    }

    holder::project::ProjectRepo project_repo(db);
    if (!project_repo.get(project_id).has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "Project not found.");
      return true;
    }

    const long long created_at = (body.contains("created_at") && !body.at("created_at").is_null())
                                     ? body.at("created_at").get<long long>()
                                     : support::now_epoch_seconds();

    holder::ai::AiThreadRepo thread_repo(db);
    if (thread_id.has_value()) {
      const auto existing = thread_repo.get(thread_id.value());
      if (!existing.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "AI thread not found.");
        return true;
      }
      if (existing->project_id != project_id) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "Thread belongs to a different project."
        );
        return true;
      }
    } else {
      holder::model::AiThread thread;
      thread.thread_id = uuid_v4();
      thread.project_id = project_id;
      std::string title = prompt.empty() ? response_text : prompt;
      if (title.size() > 80) title = title.substr(0, 80);
      if (title.empty()) title = "Captured response";
      thread.title = title;
      thread.created_at = created_at;
      thread.updated_at = created_at;
      thread_repo.create(thread);
      holder::ai::persist_ai_thread(db, thread, "Add AI thread metadata");
      thread_id = thread.thread_id;
    }

    holder::ai::AiMessageRepo msg_repo(db, fts);
    holder::model::AiMessage user_msg;
    user_msg.message_id = uuid_v4();
    user_msg.thread_id = thread_id.value();
    user_msg.role = "user";
    user_msg.source = source;
    user_msg.content = prompt;
    user_msg.created_at = created_at;
    msg_repo.append(user_msg);

    holder::model::AiMessage assistant_msg;
    assistant_msg.message_id = uuid_v4();
    assistant_msg.thread_id = thread_id.value();
    assistant_msg.role = "assistant";
    assistant_msg.source = source;
    assistant_msg.provider = provider;
    assistant_msg.model = model;
    assistant_msg.content = response_text;
    assistant_msg.created_at = created_at;

    nlohmann::json meta;
    if (body.contains("url") && !body.at("url").is_null()) {
      meta["url"] = body.at("url").get<std::string>();
    }
    if (body.contains("context") && !body.at("context").is_null()) {
      meta["context"] = body.at("context");
    }
    if (!meta.empty()) {
      assistant_msg.meta_json = meta.dump();
    }
    msg_repo.append(assistant_msg);

    nlohmann::json payload;
    payload["ok"] = true;
    payload["data"] = {
        {"thread_id", thread_id.value()},
        {"user_message_id", user_msg.message_id},
        {"assistant_message_id", assistant_msg.message_id},
    };
    res = support::json_response(http::status::created, payload);
  } catch (const std::exception& ex) {
    const std::string msg = ex.what();
    if (msg.rfind("conflict:", 0) == 0) {
      res = support::error_response(http::status::conflict, "conflict", msg);
    } else {
      res = support::error_response(http::status::bad_request, "bad_request", msg);
    }
  }
  return true;
}

} // namespace holder::api::routes::ai::messages
