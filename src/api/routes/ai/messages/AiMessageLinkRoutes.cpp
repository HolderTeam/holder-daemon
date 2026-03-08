#include "api/routes/ai/messages/AiMessageLinkRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "store/ResourceRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes::ai::messages {
namespace {

namespace http = boost::beast::http;

bool should_include_link_target(holder::store::CardRepo& card_repo,
                                holder::store::AiMessageRepo& msg_repo,
                                const holder::model::CardLink& link,
                                bool include_deleted) {
  if (include_deleted) return true;
  if (link.to_type == "card") {
    const auto target = card_repo.get(link.to_card_id);
    return target.has_value() && !target->deleted_at.has_value();
  }
  if (link.to_type == "ai_message") {
    const auto target = msg_repo.get(link.to_card_id);
    return target.has_value() && !target->deleted_at.has_value();
  }
  return true;
}

bool should_include_backlink_source(holder::store::CardRepo& card_repo,
                                    holder::store::AiMessageRepo& msg_repo,
                                    const holder::model::CardLink& link,
                                    bool include_deleted) {
  if (include_deleted) return true;
  const auto as_card = card_repo.get(link.from_card_id);
  if (as_card.has_value()) {
    return !as_card->deleted_at.has_value();
  }
  const auto as_msg = msg_repo.get(link.from_card_id);
  if (as_msg.has_value()) {
    return !as_msg->deleted_at.has_value();
  }
  return false;
}

bool validate_link_target(holder::store::Db& db,
                          const std::string& project_id,
                          const std::string& to_id,
                          const std::string& to_type_raw,
                          std::string& error) {
  if (to_id.empty()) {
    error = "Missing to_card_id.";
    return false;
  }

  const std::string to_type = to_type_raw.empty() ? "card" : to_type_raw;
  if (to_type == "card") {
    holder::store::CardRepo repo(db);
    const auto target = repo.get(to_id);
    if (!target.has_value()) {
      error = "Target card not found.";
      return false;
    }
    if (target->project_id != project_id) {
      error = "Target card is in a different project.";
      return false;
    }
    return true;
  }
  if (to_type == "ai_thread") {
    holder::store::AiThreadRepo repo(db);
    const auto target = repo.get(to_id);
    if (!target.has_value()) {
      error = "Target ai thread not found.";
      return false;
    }
    if (target->project_id != project_id) {
      error = "Target ai thread is in a different project.";
      return false;
    }
    return true;
  }
  if (to_type == "resource") {
    holder::store::ResourceRepo repo(db);
    const auto target = repo.get(to_id);
    if (!target.has_value()) {
      error = "Target resource not found.";
      return false;
    }
    if (target->project_id != project_id) {
      error = "Target resource is in a different project.";
      return false;
    }
    return true;
  }
  if (to_type == "ai_message") {
    holder::store::AiMessageRepo repo(db, nullptr);
    const auto message = repo.get(to_id);
    if (!message.has_value()) {
      error = "Target ai message not found.";
      return false;
    }
    holder::store::AiThreadRepo thread_repo(db);
    const auto thread = thread_repo.get(message->thread_id);
    if (!thread.has_value()) {
      error = "Target ai message thread not found.";
      return false;
    }
    if (thread->project_id != project_id) {
      error = "Target ai message is in a different project.";
      return false;
    }
    return true;
  }

  error = "Unsupported to_type.";
  return false;
}

} // namespace

bool handle_ai_message_link_routes(const std::string& path,
                                   const http::request<http::string_body>& req,
                                   http::response<http::string_body>& res,
                                   holder::store::Db& db,
                                   holder::index::FtsIndexer* fts,
                                   const std::function<std::string(const std::string&)>& param_get) {
  if (path.rfind("/ai/messages/", 0) != 0) {
    return false;
  }
  const std::string rest = path.substr(std::string("/ai/messages/").size());
  const auto slash = rest.find('/');
  if (slash == std::string::npos) {
    return false;
  }

  const std::string message_id = rest.substr(0, slash);
  const std::string tail = rest.substr(slash);
  if (message_id.empty()) {
    res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    return true;
  }

  if (tail == "/links") {
    try {
      holder::store::AiMessageRepo message_repo(db, fts);
      const auto msg_opt = message_repo.get(message_id);
      if (!msg_opt.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "AI message not found.");
        return true;
      }

      holder::store::AiThreadRepo thread_repo(db);
      const auto thread_opt = thread_repo.get(msg_opt->thread_id);
      if (!thread_opt.has_value()) {
        res = support::error_response(http::status::bad_request, "bad_request", "Thread not found.");
        return true;
      }

      const std::string project_id = thread_opt->project_id;
      holder::store::LinkRepo link_repo(db);
      if (req.method() == http::verb::get) {
        holder::store::CardRepo card_repo(db);
        holder::store::AiMessageRepo msg_repo(db, fts);
        const bool include_deleted = !param_get("include_deleted").empty() &&
                                     param_get("include_deleted") != "0";
        const auto links = link_repo.list_outgoing(project_id, message_id);
        nlohmann::json data = nlohmann::json::array();
        for (const auto& link : links) {
          if (!should_include_link_target(card_repo, msg_repo, link, include_deleted)) {
            continue;
          }
          nlohmann::json item;
          item["from_card_id"] = link.from_card_id;
          item["to_card_id"] = link.to_card_id;
          item["to_type"] = link.to_type;
          item["kind"] = link.kind;
          item["created_at"] = link.created_at;
          item["label"] =
              link.label.has_value() ? nlohmann::json(link.label.value()) : nlohmann::json(nullptr);
          data.push_back(std::move(item));
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = support::json_response(http::status::ok, payload);
      } else if (req.method() == http::verb::post) {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("to_card_id")) {
          res = support::error_response(http::status::bad_request, "bad_request", "Missing to_card_id.");
        } else {
          holder::model::CardLink link;
          link.project_id = project_id;
          link.from_card_id = message_id;
          link.to_card_id = body.at("to_card_id").get<std::string>();
          if (body.contains("to_type") && !body.at("to_type").is_null()) {
            link.to_type = body.at("to_type").get<std::string>();
          }
          if (link.to_type.empty()) {
            link.to_type = "card";
          }
          if (body.contains("kind") && !body.at("kind").is_null()) {
            link.kind = body.at("kind").get<std::string>();
          }
          if (link.kind.empty()) {
            link.kind = "ref";
          }
          if (body.contains("label") && !body.at("label").is_null()) {
            link.label = body.at("label").get<std::string>();
          }
          if (body.contains("created_at") && !body.at("created_at").is_null()) {
            link.created_at = body.at("created_at").get<long long>();
          } else {
            link.created_at = support::now_epoch_seconds();
          }
          std::string validation_error;
          if (!validate_link_target(db, project_id, link.to_card_id, link.to_type, validation_error)) {
            res = support::error_response(http::status::bad_request, "bad_request", validation_error);
          } else {
            link_repo.upsert_links(project_id, message_id, {link});
            message_repo.update_links(message_id);

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {
                {"from_card_id", link.from_card_id},
                {"to_card_id", link.to_card_id},
                {"to_type", link.to_type},
                {"kind", link.kind},
                {"created_at", link.created_at},
                {"label", link.label.has_value() ? nlohmann::json(link.label.value())
                                                 : nlohmann::json(nullptr)},
            };
            res = support::json_response(http::status::created, payload);
          }
        }
      } else if (req.method() == http::verb::delete_) {
        std::optional<std::string> to_card_id;
        std::optional<std::string> to_type;
        std::optional<std::string> kind;
        if (!req.body().empty()) {
          const auto body = nlohmann::json::parse(req.body());
          if (body.contains("to_card_id") && !body.at("to_card_id").is_null()) {
            to_card_id = body.at("to_card_id").get<std::string>();
          }
          if (body.contains("to_type") && !body.at("to_type").is_null()) {
            to_type = body.at("to_type").get<std::string>();
          }
          if (body.contains("kind") && !body.at("kind").is_null()) {
            kind = body.at("kind").get<std::string>();
          }
        }
        if (to_card_id.has_value()) {
          link_repo.delete_link(project_id, message_id, to_card_id.value(), to_type, kind);
        } else {
          link_repo.delete_links_from(project_id, message_id);
        }
        message_repo.update_links(message_id);

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"message_id", message_id}};
        res = support::json_response(http::status::ok, payload);
      } else {
        res = support::error_response(
            http::status::method_not_allowed, "method_not_allowed", "Method not allowed.");
      }
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (tail == "/backlinks") {
    try {
      holder::store::AiMessageRepo message_repo(db, fts);
      const auto msg_opt = message_repo.get(message_id);
      if (!msg_opt.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "AI message not found.");
      } else if (req.method() != http::verb::get) {
        res = support::error_response(
            http::status::method_not_allowed, "method_not_allowed", "Method not allowed.");
      } else {
        holder::store::AiThreadRepo thread_repo(db);
        const auto thread_opt = thread_repo.get(msg_opt->thread_id);
        if (!thread_opt.has_value()) {
          res = support::error_response(http::status::bad_request, "bad_request", "Thread not found.");
        } else {
          const std::string project_id = thread_opt->project_id;
          holder::store::LinkRepo link_repo(db);
          holder::store::CardRepo card_repo(db);
          holder::store::AiMessageRepo msg_repo(db, fts);
          const bool include_deleted = !param_get("include_deleted").empty() &&
                                       param_get("include_deleted") != "0";
          const auto links = link_repo.list_backlinks_typed(project_id, message_id, "ai_message");
          nlohmann::json data = nlohmann::json::array();
          for (const auto& link : links) {
            if (!should_include_backlink_source(card_repo, msg_repo, link, include_deleted)) {
              continue;
            }
            nlohmann::json item;
            item["from_card_id"] = link.from_card_id;
            item["to_card_id"] = link.to_card_id;
            item["to_type"] = link.to_type;
            item["kind"] = link.kind;
            item["created_at"] = link.created_at;
            item["label"] =
                link.label.has_value() ? nlohmann::json(link.label.value()) : nlohmann::json(nullptr);
            data.push_back(std::move(item));
          }
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = support::json_response(http::status::ok, payload);
        }
      }
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes::ai::messages
