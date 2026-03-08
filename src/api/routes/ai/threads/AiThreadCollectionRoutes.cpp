#include "api/routes/ai/threads/AiThreadCollectionRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "ai/AiThreadRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace holder::api::routes::ai::threads {
namespace {

namespace http = boost::beast::http;

nlohmann::json ai_thread_to_json(const holder::model::AiThread& thread) {
  nlohmann::json item;
  item["thread_id"] = thread.thread_id;
  item["project_id"] = thread.project_id;
  item["title"] = thread.title;
  item["created_at"] = thread.created_at;
  item["updated_at"] = thread.updated_at;
  item["card_id"] =
      thread.card_id.has_value() ? nlohmann::json(thread.card_id.value()) : nlohmann::json(nullptr);
  return item;
}

} // namespace

bool handle_ai_thread_collection_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::store::Db& db,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/ai/threads" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      return true;
    }

    try {
      holder::ai::AiThreadRepo repo(db);
      const auto threads = repo.list(project_id);
      nlohmann::json data = nlohmann::json::array();
      for (const auto& thread : threads) {
        data.push_back(ai_thread_to_json(thread));
      }
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = data;
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path == "/ai/threads" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("project_id") || !body.contains("title")) {
        res = support::error_response(http::status::bad_request, "bad_request", "Missing required fields.");
      } else {
        holder::model::AiThread thread;
        if (body.contains("thread_id") && !body.at("thread_id").is_null()) {
          thread.thread_id = body.at("thread_id").get<std::string>();
        }
        if (thread.thread_id.empty()) {
          thread.thread_id = uuid_v4();
        }
        thread.project_id = body.at("project_id").get<std::string>();
        thread.title = body.at("title").get<std::string>();
        if (body.contains("card_id") && !body.at("card_id").is_null()) {
          thread.card_id = body.at("card_id").get<std::string>();
        }
        if (body.contains("created_at") && !body.at("created_at").is_null()) {
          thread.created_at = body.at("created_at").get<long long>();
        }
        if (body.contains("updated_at") && !body.at("updated_at").is_null()) {
          thread.updated_at = body.at("updated_at").get<long long>();
        }
        if (thread.created_at <= 0) {
          thread.created_at = support::now_epoch_seconds();
        }
        if (thread.updated_at <= 0) {
          thread.updated_at = thread.created_at;
        }

        holder::ai::AiThreadRepo repo(db);
        repo.create(thread);

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"thread_id", thread.thread_id}};
        res = support::json_response(http::status::created, payload);
      }
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

  return false;
}

} // namespace holder::api::routes::ai::threads
