#include "api/routes/AiMessageRoutes.h"

#include "store/AiMessageRepo.h"
#include "store/AiThreadRepo.h"
#include "store/CardRepo.h"
#include "store/LinkRepo.h"
#include "store/ProjectRepo.h"
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

bool handle_ai_message_routes(const std::string& path,
                              const http::request<http::string_body>& req,
                              http::response<http::string_body>& res,
                              holder::store::Db& db,
                              holder::index::FtsIndexer* fts,
                              const std::function<std::string()>& uuid_v4,
                              const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/ai/messages/capture" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("project_id") || !body.contains("prompt") || !body.contains("response")) {
        res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
      } else {
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

        holder::store::ProjectRepo project_repo(db);
        if (!project_repo.get(project_id).has_value()) {
          res = error_response(http::status::not_found, "not_found", "Project not found.");
        } else {
          const long long created_at = (body.contains("created_at") && !body.at("created_at").is_null())
                                           ? body.at("created_at").get<long long>()
                                           : now_epoch_seconds();

          holder::store::AiThreadRepo thread_repo(db);
          if (thread_id.has_value()) {
            const auto existing = thread_repo.get(thread_id.value());
            if (!existing.has_value()) {
              res = error_response(http::status::not_found, "not_found", "AI thread not found.");
            } else if (existing->project_id != project_id) {
              res = error_response(http::status::bad_request,
                                   "bad_request",
                                   "Thread belongs to a different project.");
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
            thread_id = thread.thread_id;
          }

          holder::store::AiMessageRepo msg_repo(db, fts);
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
          res = json_response(http::status::created, payload);
        }
      }
    } catch (const std::exception& ex) {
      const std::string msg = ex.what();
      if (msg.rfind("conflict:", 0) == 0) {
        res = error_response(http::status::conflict, "conflict", msg);
      } else {
        res = error_response(http::status::bad_request, "bad_request", msg);
      }
    }
    return true;
  }

  if (path == "/ai/messages" && req.method() == http::verb::get) {
    const std::string thread_id = param_get("thread_id");
    const std::string include_deleted_raw = param_get("include_deleted");
    if (thread_id.empty()) {
      res = error_response(http::status::bad_request, "bad_request", "Missing thread_id.");
    } else {
      try {
        holder::store::AiMessageRepo repo(db, fts);
        const auto messages = repo.list_by_thread(thread_id);
        nlohmann::json data = nlohmann::json::array();
        for (const auto& msg : messages) {
          if (include_deleted_raw.empty() || include_deleted_raw == "0") {
            if (msg.deleted_at.has_value()) {
              continue;
            }
          }
          nlohmann::json item;
          item["message_id"] = msg.message_id;
          item["thread_id"] = msg.thread_id;
          item["role"] = msg.role;
          item["source"] = msg.source;
          item["provider"] = msg.provider.has_value() ? nlohmann::json(msg.provider.value())
                                                      : nlohmann::json(nullptr);
          item["model"] = msg.model.has_value() ? nlohmann::json(msg.model.value())
                                                : nlohmann::json(nullptr);
          item["content"] = msg.content;
          item["created_at"] = msg.created_at;
          item["deleted_at"] = msg.deleted_at.has_value() ? nlohmann::json(msg.deleted_at.value())
                                                          : nlohmann::json(nullptr);
          item["prompt_hash"] = msg.prompt_hash.has_value() ? nlohmann::json(msg.prompt_hash.value())
                                                            : nlohmann::json(nullptr);
          item["meta_json"] = msg.meta_json.has_value() ? nlohmann::json(msg.meta_json.value())
                                                        : nlohmann::json(nullptr);
          data.push_back(std::move(item));
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = error_response(http::status::bad_request, "bad_request", ex.what());
      }
    }
    return true;
  }

  if (path == "/ai/messages" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("thread_id") || !body.contains("role") || !body.contains("source") ||
          !body.contains("content")) {
        res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
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
          msg.created_at = now_epoch_seconds();
        }

        holder::store::AiMessageRepo repo(db, fts);
        repo.append(msg);

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"message_id", msg.message_id}};
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
    return true;
  }

  if (path.rfind("/ai/messages/", 0) == 0) {
    const std::string rest = path.substr(std::string("/ai/messages/").size());
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
      const std::string message_id = rest.substr(0, slash);
      const std::string tail = rest.substr(slash);
      if (message_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (tail == "/links") {
        try {
          holder::store::AiMessageRepo message_repo(db, fts);
          const auto msg_opt = message_repo.get(message_id);
          if (!msg_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "AI message not found.");
          } else {
            holder::store::AiThreadRepo thread_repo(db);
            const auto thread_opt = thread_repo.get(msg_opt->thread_id);
            if (!thread_opt.has_value()) {
              res = error_response(http::status::bad_request, "bad_request", "Thread not found.");
            } else {
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
                    link.created_at = now_epoch_seconds();
                  }
                  std::string validation_error;
                  if (!validate_link_target(db,
                                            project_id,
                                            link.to_card_id,
                                            link.to_type,
                                            validation_error)) {
                    res = error_response(http::status::bad_request, "bad_request", validation_error);
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
                  link_repo.delete_link(project_id, message_id, to_card_id.value(), to_type, kind);
                } else {
                  link_repo.delete_links_from(project_id, message_id);
                }
                message_repo.update_links(message_id);

                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = {{"message_id", message_id}};
                res = json_response(http::status::ok, payload);
              } else {
                res = error_response(http::status::method_not_allowed,
                                     "method_not_allowed",
                                     "Method not allowed.");
              }
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (tail == "/backlinks") {
        try {
          holder::store::AiMessageRepo message_repo(db, fts);
          const auto msg_opt = message_repo.get(message_id);
          if (!msg_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "AI message not found.");
          } else if (req.method() != http::verb::get) {
            res = error_response(http::status::method_not_allowed,
                                 "method_not_allowed",
                                 "Method not allowed.");
          } else {
            holder::store::AiThreadRepo thread_repo(db);
            const auto thread_opt = thread_repo.get(msg_opt->thread_id);
            if (!thread_opt.has_value()) {
              res = error_response(http::status::bad_request, "bad_request", "Thread not found.");
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
                item["label"] = link.label.has_value() ? nlohmann::json(link.label.value())
                                                       : nlohmann::json(nullptr);
                data.push_back(std::move(item));
              }
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = data;
              res = json_response(http::status::ok, payload);
            }
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
            holder::store::AiMessageRepo repo(db, fts);
            repo.restore(message_id);
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"message_id", message_id}};
            res = json_response(http::status::ok, payload);
          } catch (const std::exception& ex) {
            res = error_response(http::status::bad_request, "bad_request", ex.what());
          }
        }
      } else {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      }
    } else {
      const std::string message_id = rest;
      if (message_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (req.method() == http::verb::delete_) {
        try {
          holder::store::AiMessageRepo repo(db, fts);
          const long long deleted_at = now_epoch_seconds();
          repo.trash(message_id, deleted_at);
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"message_id", message_id}};
          res = json_response(http::status::ok, payload);
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::post && rest.size() > 0) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (req.method() == http::verb::patch) {
        try {
          const auto body = nlohmann::json::parse(req.body());
          holder::store::AiMessageRepo repo(db, fts);
          const auto msg_opt = repo.get(message_id);
          if (!msg_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "AI message not found.");
          } else {
            auto msg = msg_opt.value();
            if (msg.deleted_at.has_value()) {
              res = error_response(http::status::bad_request, "bad_request", "AI message is deleted.");
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
              res = json_response(http::status::ok, payload);
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else {
        try {
          holder::store::AiMessageRepo repo(db, fts);
          const auto msg_opt = repo.get(message_id);
          if (!msg_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "AI message not found.");
          } else {
            const auto& msg = msg_opt.value();
            if (msg.deleted_at.has_value()) {
              res = error_response(http::status::not_found, "not_found", "AI message not found.");
            } else {
              nlohmann::json data;
              data["message_id"] = msg.message_id;
              data["thread_id"] = msg.thread_id;
              data["role"] = msg.role;
              data["source"] = msg.source;
              data["provider"] = msg.provider.has_value() ? nlohmann::json(msg.provider.value())
                                                          : nlohmann::json(nullptr);
              data["model"] = msg.model.has_value() ? nlohmann::json(msg.model.value())
                                                    : nlohmann::json(nullptr);
              data["content"] = msg.content;
              data["created_at"] = msg.created_at;
              data["prompt_hash"] = msg.prompt_hash.has_value() ? nlohmann::json(msg.prompt_hash.value())
                                                                : nlohmann::json(nullptr);
              data["meta_json"] = msg.meta_json.has_value() ? nlohmann::json(msg.meta_json.value())
                                                            : nlohmann::json(nullptr);

              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = data;
              res = json_response(http::status::ok, payload);
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
