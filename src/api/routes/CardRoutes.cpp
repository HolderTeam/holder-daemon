#include "api/routes/CardRoutes.h"

#include "core/CardPaths.h"
#include "store/AiMessageRepo.h"
#include "store/AiThreadRepo.h"
#include "store/CardRepo.h"
#include "store/LinkRepo.h"
#include "store/ResourceRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

http::response<http::string_body> json_response(http::status status,
                                                const nlohmann::json& payload) {
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, "application/json");
  res.keep_alive(false);
  res.body() = payload.dump();
  res.prepare_payload();
  return res;
}

http::response<http::string_body> error_response(http::status status,
                                                 std::string code,
                                                 std::string message) {
  nlohmann::json j;
  j["ok"] = false;
  j["error"] = {{"code", std::move(code)}, {"message", std::move(message)}};
  return json_response(status, j);
}

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

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

bool handle_card_routes(const std::string& path,
                        const http::request<http::string_body>& req,
                        http::response<http::string_body>& res,
                        holder::store::Db& db,
                        holder::store::CardStore* card_store,
                        holder::index::FtsIndexer* fts,
                        const std::function<std::string()>& uuid_v4,
                        const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/cards" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    const std::string parent_raw = param_get("parent_card_id");
    const std::string include_deleted_raw = param_get("include_deleted");
    if (project_id.empty()) {
      res = error_response(http::status::bad_request, "bad_request", "Missing project_id.");
    } else {
      try {
        holder::store::CardRepo repo(db);
        std::optional<std::string> parent;
        if (!parent_raw.empty()) {
          parent = parent_raw;
        }
        const auto cards = repo.list(project_id, parent);
        nlohmann::json data = nlohmann::json::array();
        for (const auto& card : cards) {
          if (!include_deleted_raw.empty() && include_deleted_raw != "0") {
            // include deleted
          } else if (card.deleted_at.has_value()) {
            continue;
          }
          nlohmann::json item;
          item["card_id"] = card.card_id;
          item["project_id"] = card.project_id;
          item["title"] = card.title;
          item["rel_path"] = card.rel_path;
          item["sort_key"] = card.sort_key;
          item["created_at"] = card.created_at;
          item["updated_at"] = card.updated_at;
          item["parent_card_id"] = card.parent_card_id.has_value()
                                       ? nlohmann::json(card.parent_card_id.value())
                                       : nlohmann::json(nullptr);
          item["deleted_at"] = card.deleted_at.has_value() ? nlohmann::json(card.deleted_at.value())
                                                           : nlohmann::json(nullptr);
          data.push_back(std::move(item));
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = error_response(http::status::internal_server_error, "error", ex.what());
      }
    }
    return true;
  }

  if (path == "/cards" && req.method() == http::verb::post) {
    if (!card_store) {
      res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
    } else {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("project_id") || !body.contains("title") || !body.contains("content")) {
          res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
        } else {
          holder::model::Card card;
          if (body.contains("card_id") && !body.at("card_id").is_null()) {
            card.card_id = body.at("card_id").get<std::string>();
          }
          if (card.card_id.empty()) {
            card.card_id = uuid_v4();
          }
          card.project_id = body.at("project_id").get<std::string>();
          card.title = body.at("title").get<std::string>();
          if (body.contains("created_at") && !body.at("created_at").is_null()) {
            card.created_at = body.at("created_at").get<long long>();
          }
          if (body.contains("updated_at") && !body.at("updated_at").is_null()) {
            card.updated_at = body.at("updated_at").get<long long>();
          }
          if (card.created_at <= 0) {
            card.created_at = now_epoch_seconds();
          }
          if (card.updated_at <= 0) {
            card.updated_at = card.created_at;
          }
          if (body.contains("parent_card_id") && !body.at("parent_card_id").is_null()) {
            card.parent_card_id = body.at("parent_card_id").get<std::string>();
          }
          if (body.contains("sort_key")) {
            card.sort_key = body.at("sort_key").get<double>();
          }
          if (body.contains("rel_path") && !body.at("rel_path").is_null()) {
            card.rel_path = body.at("rel_path").get<std::string>();
          }

          const std::string content = body.at("content").get<std::string>();
          if (card.rel_path.empty()) {
            card.rel_path = holder::core::card_rel_path(card.card_id);
          }
          card_store->create(card, content);

          nlohmann::json data;
          data["card_id"] = card.card_id;
          data["rel_path"] = card.rel_path;
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = json_response(http::status::created, payload);
        }
      } catch (const std::exception& ex) {
        const std::string msg = ex.what();
        if (msg.rfind("conflict:", 0) == 0) {
          res = error_response(http::status::conflict, "conflict", msg);
        } else {
          res = error_response(http::status::bad_request, "bad_request", msg);
        }
      }
    }
    return true;
  }

  if (path.rfind("/cards/", 0) == 0) {
    const std::string rest = path.substr(std::string("/cards/").size());
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
      const std::string card_id = rest.substr(0, slash);
      const std::string tail = rest.substr(slash);
      if (card_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (!card_store) {
        res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
      } else if (tail == "/links") {
        try {
          const auto card_opt = card_store->get(card_id);
          if (!card_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Card not found.");
          } else {
            const auto& card = card_opt.value();
            holder::store::LinkRepo repo(db);
            if (req.method() == http::verb::get) {
              holder::store::CardRepo card_repo(db);
              holder::store::AiMessageRepo msg_repo(db, fts);
              const bool include_deleted = !param_get("include_deleted").empty() &&
                                           param_get("include_deleted") != "0";
              const auto links = repo.list_outgoing(card.project_id, card.card_id);
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
                item["label"] = link.label.has_value() ? nlohmann::json(link.label.value())
                                                       : nlohmann::json(nullptr);
                data.push_back(std::move(item));
              }
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = data;
              res = json_response(http::status::ok, payload);
            } else if (req.method() == http::verb::post) {
              const auto body = nlohmann::json::parse(req.body());
              if (!body.contains("to_card_id")) {
                res = error_response(http::status::bad_request, "bad_request", "Missing to_card_id.");
              } else {
                holder::model::CardLink link;
                link.project_id = card.project_id;
                link.from_card_id = card.card_id;
                link.to_card_id = body.at("to_card_id").get<std::string>();
                if (body.contains("to_type") && !body.at("to_type").is_null()) {
                  link.to_type = body.at("to_type").get<std::string>();
                }
                if (body.contains("kind") && !body.at("kind").is_null()) {
                  link.kind = body.at("kind").get<std::string>();
                }
                if (link.to_type.empty()) {
                  link.to_type = "card";
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
                  link.created_at = now_epoch_seconds();
                }
                std::string validation_error;
                if (!validate_link_target(db,
                                          card.project_id,
                                          link.to_card_id,
                                          link.to_type,
                                          validation_error)) {
                  res = error_response(http::status::bad_request, "bad_request", validation_error);
                } else {
                  repo.upsert_links(card.project_id, card.card_id, {link});
                  card_store->update_links(card.card_id, now_epoch_seconds());

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
                  res = json_response(http::status::created, payload);
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
                repo.delete_link(card.project_id, card.card_id, to_card_id.value(), to_type, kind);
              } else {
                repo.delete_links_from(card.project_id, card.card_id);
              }
              card_store->update_links(card.card_id, now_epoch_seconds());

              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"card_id", card.card_id}};
              res = json_response(http::status::ok, payload);
            } else {
              res = error_response(http::status::method_not_allowed,
                                   "method_not_allowed",
                                   "Method not allowed.");
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (tail == "/backlinks") {
        try {
          const auto card_opt = card_store->get(card_id);
          if (!card_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Card not found.");
          } else if (req.method() != http::verb::get) {
            res = error_response(http::status::method_not_allowed,
                                 "method_not_allowed",
                                 "Method not allowed.");
          } else {
            const auto& card = card_opt.value();
            holder::store::LinkRepo repo(db);
            holder::store::CardRepo card_repo(db);
            holder::store::AiMessageRepo msg_repo(db, fts);
            const bool include_deleted = !param_get("include_deleted").empty() &&
                                         param_get("include_deleted") != "0";
            const auto links = repo.list_backlinks_typed(card.project_id, card.card_id, "card");
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
              item["label"] = link.label.has_value() ? nlohmann::json(link.label.value())
                                                     : nlohmann::json(nullptr);
              data.push_back(std::move(item));
            }
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = data;
            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (tail == "/restore") {
        if (req.method() != http::verb::post) {
          res = error_response(http::status::method_not_allowed,
                               "method_not_allowed",
                               "Method not allowed.");
        } else {
          try {
            const long long updated_at = now_epoch_seconds();
            card_store->restore(card_id, updated_at);
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"card_id", card_id}};
            res = json_response(http::status::ok, payload);
          } catch (const std::exception& ex) {
            res = error_response(http::status::bad_request, "bad_request", ex.what());
          }
        }
      } else {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      }
    } else {
      const std::string card_id = rest;
      if (card_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (!card_store) {
        res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
      } else if (req.method() == http::verb::get) {
        try {
          const auto card_opt = card_store->get(card_id);
          if (!card_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Card not found.");
          } else {
            const auto& card = card_opt.value();
            if (card.deleted_at.has_value()) {
              res = error_response(http::status::not_found, "not_found", "Card not found.");
            } else {
              const auto content_opt = card_store->get_content(card);
              if (!content_opt.has_value()) {
                res = error_response(http::status::not_found, "not_found", "Card content missing.");
              } else {
                nlohmann::json data;
                data["card_id"] = card.card_id;
                data["project_id"] = card.project_id;
                data["title"] = card.title;
                data["rel_path"] = card.rel_path;
                data["sort_key"] = card.sort_key;
                data["created_at"] = card.created_at;
                data["updated_at"] = card.updated_at;
                data["parent_card_id"] = card.parent_card_id.has_value()
                                             ? nlohmann::json(card.parent_card_id.value())
                                             : nlohmann::json(nullptr);
                data["deleted_at"] = card.deleted_at.has_value() ? nlohmann::json(card.deleted_at.value())
                                                                 : nlohmann::json(nullptr);
                data["content"] = content_opt.value();

                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = data;
                res = json_response(http::status::ok, payload);
              }
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::internal_server_error, "error", ex.what());
        }
      } else if (req.method() == http::verb::patch) {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("content") || !body.contains("updated_at")) {
            res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
          } else {
            std::optional<std::string> title;
            if (body.contains("title") && !body.at("title").is_null()) {
              title = body.at("title").get<std::string>();
            }
            const std::string content = body.at("content").get<std::string>();
            const long long updated_at = body.at("updated_at").get<long long>();

            card_store->update_content(card_id, content, title, updated_at);

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"card_id", card_id}};
            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::delete_) {
        try {
          const long long deleted_at = now_epoch_seconds();
          card_store->trash(card_id, deleted_at);
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"card_id", card_id}};
          res = json_response(http::status::ok, payload);
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::post && rest.size() > 0) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      }
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
