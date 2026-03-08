#include "api/routes/ai/messages/AiMessageCrudRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "ai/AiMessageRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace holder::api::routes::ai::messages {
namespace {

namespace http = boost::beast::http;

nlohmann::json ai_message_to_json(const holder::model::AiMessage& msg) {
  nlohmann::json data;
  data["message_id"] = msg.message_id;
  data["thread_id"] = msg.thread_id;
  data["role"] = msg.role;
  data["source"] = msg.source;
  data["provider"] =
      msg.provider.has_value() ? nlohmann::json(msg.provider.value()) : nlohmann::json(nullptr);
  data["model"] = msg.model.has_value() ? nlohmann::json(msg.model.value()) : nlohmann::json(nullptr);
  data["content"] = msg.content;
  data["created_at"] = msg.created_at;
  data["deleted_at"] =
      msg.deleted_at.has_value() ? nlohmann::json(msg.deleted_at.value()) : nlohmann::json(nullptr);
  data["prompt_hash"] =
      msg.prompt_hash.has_value() ? nlohmann::json(msg.prompt_hash.value()) : nlohmann::json(nullptr);
  data["meta_json"] =
      msg.meta_json.has_value() ? nlohmann::json(msg.meta_json.value()) : nlohmann::json(nullptr);
  return data;
}

} // namespace

bool handle_ai_message_crud_routes(const std::string& path,
                                   const http::request<http::string_body>& req,
                                   http::response<http::string_body>& res,
                                   holder::store::Db& db,
                                   holder::index::FtsIndexer* fts,
                                   const std::function<std::string()>& uuid_v4,
                                   const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/ai/messages" && req.method() == http::verb::get) {
    const std::string thread_id = param_get("thread_id");
    const std::string include_deleted_raw = param_get("include_deleted");
    if (thread_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing thread_id.");
      return true;
    }

    try {
      holder::ai::AiMessageRepo repo(db, fts);
      const auto messages = repo.list_by_thread(thread_id);
      nlohmann::json data = nlohmann::json::array();
      for (const auto& msg : messages) {
        if ((include_deleted_raw.empty() || include_deleted_raw == "0") && msg.deleted_at.has_value()) {
          continue;
        }
        data.push_back(ai_message_to_json(msg));
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

  if (path == "/ai/messages" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("thread_id") || !body.contains("role") || !body.contains("source") ||
          !body.contains("content")) {
        res = support::error_response(http::status::bad_request, "bad_request", "Missing required fields.");
      } else {
        holder::model::AiMessage msg;
        if (body.contains("message_id") && !body.at("message_id").is_null()) {
          msg.message_id = body.at("message_id").get<std::string>();
        }
        if (msg.message_id.empty()) {
          msg.message_id = uuid_v4();
        }
        msg.thread_id = body.at("thread_id").get<std::string>();
        msg.role = body.at("role").get<std::string>();
        msg.source = body.at("source").get<std::string>();
        msg.content = body.at("content").get<std::string>();
        if (body.contains("provider") && !body.at("provider").is_null()) {
          msg.provider = body.at("provider").get<std::string>();
        }
        if (body.contains("model") && !body.at("model").is_null()) {
          msg.model = body.at("model").get<std::string>();
        }
        if (body.contains("prompt_hash") && !body.at("prompt_hash").is_null()) {
          msg.prompt_hash = body.at("prompt_hash").get<std::string>();
        }
        if (body.contains("meta_json") && !body.at("meta_json").is_null()) {
          msg.meta_json = body.at("meta_json").get<std::string>();
        }
        if (body.contains("created_at") && !body.at("created_at").is_null()) {
          msg.created_at = body.at("created_at").get<long long>();
        }
        if (msg.created_at <= 0) {
          msg.created_at = support::now_epoch_seconds();
        }

        holder::ai::AiMessageRepo repo(db, fts);
        repo.append(msg);

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"message_id", msg.message_id}};
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

  if (path.rfind("/ai/messages/", 0) != 0) {
    return false;
  }
  const std::string rest = path.substr(std::string("/ai/messages/").size());
  if (rest.empty()) {
    return false;
  }
  const auto slash = rest.find('/');
  if (slash != std::string::npos) {
    const std::string message_id = rest.substr(0, slash);
    const std::string tail = rest.substr(slash);
    if (message_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      return true;
    }
    if (tail != "/restore") {
      return false;
    }
    if (req.method() != http::verb::post) {
      res = support::error_response(
          http::status::method_not_allowed, "method_not_allowed", "Method not allowed.");
      return true;
    }
    try {
      holder::ai::AiMessageRepo repo(db, fts);
      repo.restore(message_id);
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {{"message_id", message_id}};
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }
  const std::string message_id = rest;

  if (req.method() == http::verb::delete_) {
    try {
      holder::ai::AiMessageRepo repo(db, fts);
      const long long deleted_at = support::now_epoch_seconds();
      repo.trash(message_id, deleted_at);
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {{"message_id", message_id}};
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (req.method() == http::verb::patch) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      holder::ai::AiMessageRepo repo(db, fts);
      const auto msg_opt = repo.get(message_id);
      if (!msg_opt.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "AI message not found.");
      } else {
        auto msg = msg_opt.value();
        if (msg.deleted_at.has_value()) {
          res = support::error_response(http::status::bad_request, "bad_request", "AI message is deleted.");
        } else {
          if (body.contains("role") && !body.at("role").is_null()) {
            msg.role = body.at("role").get<std::string>();
          }
          if (body.contains("source") && !body.at("source").is_null()) {
            msg.source = body.at("source").get<std::string>();
          }
          if (body.contains("provider")) {
            if (body.at("provider").is_null()) {
              msg.provider.reset();
            } else {
              msg.provider = body.at("provider").get<std::string>();
            }
          }
          if (body.contains("model")) {
            if (body.at("model").is_null()) {
              msg.model.reset();
            } else {
              msg.model = body.at("model").get<std::string>();
            }
          }
          if (body.contains("content") && !body.at("content").is_null()) {
            msg.content = body.at("content").get<std::string>();
          }
          if (body.contains("created_at") && !body.at("created_at").is_null()) {
            msg.created_at = body.at("created_at").get<long long>();
          }
          if (body.contains("prompt_hash")) {
            if (body.at("prompt_hash").is_null()) {
              msg.prompt_hash.reset();
            } else {
              msg.prompt_hash = body.at("prompt_hash").get<std::string>();
            }
          }
          if (body.contains("meta_json")) {
            if (body.at("meta_json").is_null()) {
              msg.meta_json.reset();
            } else {
              msg.meta_json = body.at("meta_json").get<std::string>();
            }
          }

          repo.update(msg);
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"message_id", message_id}};
          res = support::json_response(http::status::ok, payload);
        }
      }
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (req.method() == http::verb::post) {
    res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    return true;
  }

  try {
    holder::ai::AiMessageRepo repo(db, fts);
    const auto msg_opt = repo.get(message_id);
    if (!msg_opt.has_value() || msg_opt->deleted_at.has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "AI message not found.");
    } else {
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = ai_message_to_json(msg_opt.value());
      res = support::json_response(http::status::ok, payload);
    }
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  }
  return true;
}

} // namespace holder::api::routes::ai::messages
