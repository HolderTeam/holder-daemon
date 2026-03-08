#include "api/routes/TrashRoutes.h"
#include "api/support/HttpResponses.h"

#include "ai/AiMessageRepo.h"
#include "card/CardRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

} // namespace

bool handle_trash_routes(const std::string& path,
                         const http::request<http::string_body>& req,
                         http::response<http::string_body>& res,
                         holder::store::Db& db,
                         holder::card::CardStore* card_store,
                         holder::index::FtsIndexer* fts,
                         const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/trash" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    const std::string type = param_get("type");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
    } else {
      try {
        nlohmann::json data = nlohmann::json::array();
        if (type.empty() || type == "card" || type == "all") {
          holder::card::CardRepo card_repo(db);
          const auto cards = card_repo.list_all(project_id);
          for (const auto& card : cards) {
            if (!card.deleted_at.has_value()) continue;
            nlohmann::json item;
            item["type"] = "card";
            item["card_id"] = card.card_id;
            item["project_id"] = card.project_id;
            item["title"] = card.title;
            item["deleted_at"] = card.deleted_at.value();
            item["rel_path"] = card.rel_path;
            data.push_back(std::move(item));
          }
        }
        if (type.empty() || type == "ai_message" || type == "all") {
          holder::ai::AiMessageRepo msg_repo(db, fts);
          const auto msgs = msg_repo.list_deleted_by_project(project_id);
          for (const auto& msg : msgs) {
            if (!msg.deleted_at.has_value()) continue;
            nlohmann::json item;
            item["type"] = "ai_message";
            item["message_id"] = msg.message_id;
            item["thread_id"] = msg.thread_id;
            item["project_id"] = project_id;
            item["role"] = msg.role;
            item["deleted_at"] = msg.deleted_at.value();
            item["created_at"] = msg.created_at;
            data.push_back(std::move(item));
          }
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

  if (path == "/trash" && req.method() == http::verb::delete_) {
    const std::string project_id = param_get("project_id");
    const std::string type = param_get("type");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
    } else {
      try {
        if ((type.empty() || type == "card" || type == "all") && card_store) {
          holder::card::CardRepo card_repo(db);
          const auto cards = card_repo.list_all(project_id);
          for (const auto& card : cards) {
            if (!card.deleted_at.has_value()) continue;
            card_store->hard_delete(card.card_id);
          }
        }
        if (type.empty() || type == "ai_message" || type == "all") {
          holder::ai::AiMessageRepo msg_repo(db, fts);
          const auto msgs = msg_repo.list_deleted_by_project(project_id);
          for (const auto& msg : msgs) {
            msg_repo.remove(msg.message_id);
          }
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"project_id", project_id}};
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    }
    return true;
  }

  if (path.rfind("/trash/", 0) == 0 && req.method() == http::verb::delete_) {
    const std::string rest = path.substr(std::string("/trash/").size());
    const auto slash = rest.find('/');
    if (slash == std::string::npos) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    } else {
      const std::string type = rest.substr(0, slash);
      const std::string id = rest.substr(slash + 1);
      if (id.empty()) {
        res = support::error_response(http::status::bad_request, "bad_request", "Missing id.");
      } else {
        try {
          if (type == "card") {
            if (!card_store) {
              res = support::error_response(http::status::not_implemented,
                                   "not_implemented",
                                   "Card store unavailable.");
            } else {
              card_store->hard_delete(id);
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"card_id", id}};
              res = support::json_response(http::status::ok, payload);
            }
          } else if (type == "ai_message") {
            holder::ai::AiMessageRepo msg_repo(db, fts);
            msg_repo.remove(id);
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"message_id", id}};
            res = support::json_response(http::status::ok, payload);
          } else {
            res = support::error_response(http::status::bad_request, "bad_request", "Unknown trash type.");
          }
        } catch (const std::exception& ex) {
          res = support::error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
