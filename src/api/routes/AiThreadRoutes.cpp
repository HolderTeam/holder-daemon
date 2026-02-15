#include "api/routes/AiThreadRoutes.h"
#include "api/support/HttpResponses.h"

#include "store/AiThreadRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

bool handle_ai_thread_routes(const std::string& path,
                             const http::request<http::string_body>& req,
                             http::response<http::string_body>& res,
                             holder::store::Db& db,
                             const std::function<std::string()>& uuid_v4,
                             const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/ai/threads" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
    } else {
      try {
        holder::store::AiThreadRepo repo(db);
        const auto threads = repo.list(project_id);
        nlohmann::json data = nlohmann::json::array();
        for (const auto& thread : threads) {
          nlohmann::json item;
          item["thread_id"] = thread.thread_id;
          item["project_id"] = thread.project_id;
          item["title"] = thread.title;
          item["created_at"] = thread.created_at;
          item["updated_at"] = thread.updated_at;
          if (thread.card_id.has_value()) {
            item["card_id"] = thread.card_id.value();
          } else {
            item["card_id"] = nullptr;
          }
          data.push_back(std::move(item));
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
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
          thread.created_at = now_epoch_seconds();
        }
        if (thread.updated_at <= 0) {
          thread.updated_at = thread.created_at;
        }

        holder::store::AiThreadRepo repo(db);
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

  if (path.rfind("/ai/threads/", 0) == 0) {
    const std::string thread_id = path.substr(std::string("/ai/threads/").size());
    if (thread_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    } else if (req.method() == http::verb::get) {
      try {
        holder::store::AiThreadRepo repo(db);
        const auto thread_opt = repo.get(thread_id);
        if (!thread_opt.has_value()) {
          res = support::error_response(http::status::not_found, "not_found", "AI thread not found.");
        } else {
          const auto& thread = thread_opt.value();
          nlohmann::json data;
          data["thread_id"] = thread.thread_id;
          data["project_id"] = thread.project_id;
          data["title"] = thread.title;
          data["created_at"] = thread.created_at;
          data["updated_at"] = thread.updated_at;
          if (thread.card_id.has_value()) {
            data["card_id"] = thread.card_id.value();
          } else {
            data["card_id"] = nullptr;
          }
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = support::json_response(http::status::ok, payload);
        }
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (req.method() == http::verb::patch) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("updated_at")) {
          res = support::error_response(http::status::bad_request, "bad_request", "Missing updated_at.");
        } else {
          std::optional<std::string> title;
          if (body.contains("title") && !body.at("title").is_null()) {
            title = body.at("title").get<std::string>();
          }
          std::optional<std::string> card_id;
          if (body.contains("card_id") && !body.at("card_id").is_null()) {
            card_id = body.at("card_id").get<std::string>();
          }
          const long long updated_at = body.at("updated_at").get<long long>();

          holder::store::AiThreadRepo repo(db);
          if (title.has_value()) {
            repo.update_title(thread_id, title.value(), updated_at);
          } else {
            repo.touch_updated(thread_id, updated_at);
          }
          if (body.contains("card_id")) {
            repo.update_card_id(thread_id, card_id);
          }

          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"thread_id", thread_id}};
          res = support::json_response(http::status::ok, payload);
        }
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (req.method() == http::verb::delete_) {
      try {
        holder::store::AiThreadRepo repo(db);
        repo.remove(thread_id);
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"thread_id", thread_id}};
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
