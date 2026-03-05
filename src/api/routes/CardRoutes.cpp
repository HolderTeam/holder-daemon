#include "api/routes/CardRoutes.h"
#include "api/support/HttpResponses.h"
#include "api/support/Time.h"

#include "core/CardPaths.h"
#include "privacy/PrivacyError.h"
#include "store/AiMessageRepo.h"
#include "store/AiThreadRepo.h"
#include "store/CardRepo.h"
#include "store/LinkRepo.h"
#include "store/ProjectRepo.h"
#include "store/ResourceRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace holder::api::routes {
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

http::response<http::string_body> privacy_error_response(
    const holder::privacy::PrivacyError& ex) {
  http::status status = http::status::bad_request;
  if (ex.code() == holder::privacy::PrivacyErrorCode::KeyringUnavailable) {
    status = http::status::service_unavailable;
  }
  return support::error_response(status,
                                 holder::privacy::privacy_error_code_name(ex.code()),
                                 ex.what());
}

std::optional<std::string> normalize_parent_id(const std::optional<std::string>& parent_card_id) {
  if (!parent_card_id.has_value()) {
    return std::nullopt;
  }
  const std::string& raw = parent_card_id.value();
  const auto start = raw.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return std::nullopt;
  }
  const auto end = raw.find_last_not_of(" \t\r\n");
  return raw.substr(start, end - start + 1);
}

std::optional<std::string> normalize_parent_id(const nlohmann::json& value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  return normalize_parent_id(std::optional<std::string>(value.get<std::string>()));
}

bool is_descendant_of(const std::unordered_map<std::string, holder::model::Card>& cards_by_id,
                      std::optional<std::string> candidate_parent_card_id,
                      const std::string& card_id) {
  int guard = 0;
  while (candidate_parent_card_id.has_value() && guard < 1024) {
    if (candidate_parent_card_id.value() == card_id) {
      return true;
    }
    const auto it = cards_by_id.find(candidate_parent_card_id.value());
    if (it == cards_by_id.end()) {
      return false;
    }
    candidate_parent_card_id = normalize_parent_id(it->second.parent_card_id);
    guard++;
  }
  return false;
}

double sort_key_around_target(const std::vector<holder::model::Card>& siblings,
                              const std::string& target_card_id,
                              bool after) {
  size_t target_index = 0;
  bool found = false;
  for (size_t i = 0; i < siblings.size(); ++i) {
    if (siblings[i].card_id == target_card_id) {
      target_index = i;
      found = true;
      break;
    }
  }
  if (!found) {
    throw std::runtime_error("invalid_target");
  }

  double left = 0.0;
  double right = 0.0;
  if (after) {
    left = siblings[target_index].sort_key;
    right = (target_index + 1 < siblings.size())
                ? siblings[target_index + 1].sort_key
                : left + 1.0;
  } else {
    right = siblings[target_index].sort_key;
    left = (target_index > 0U) ? siblings[target_index - 1].sort_key : right - 1.0;
  }
  if (right - left < 0.0001) {
    return after ? right + 1.0 : left - 1.0;
  }
  return (left + right) / 2.0;
}

std::optional<int> parse_limit_param(const std::string& limit_raw,
                                     http::response<http::string_body>& res) {
  int limit = 200;
  if (!limit_raw.empty()) {
    try {
      const long parsed = std::stol(limit_raw);
      if (parsed <= 0 || parsed > 5000) {
        res = support::error_response(http::status::bad_request,
                                      "bad_request",
                                      "limit must be between 1 and 5000.");
        return std::nullopt;
      }
      limit = static_cast<int>(parsed);
    } catch (const std::exception&) {
      res = support::error_response(http::status::bad_request,
                                    "bad_request",
                                    "Invalid limit. Expected integer.");
      return std::nullopt;
    }
  }
  return limit;
}

http::response<http::string_body> recent_cards_response(holder::store::Db& db,
                                                        const std::string& project_id,
                                                        int limit) {
  holder::store::CardRepo repo(db);
  auto cards = repo.list_all(project_id);
  std::vector<holder::model::Card> visible;
  visible.reserve(cards.size());
  for (const auto& c : cards) {
    if (!c.deleted_at.has_value()) {
      visible.push_back(c);
    }
  }
  std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) {
    if (a.updated_at > b.updated_at) return true;
    if (a.updated_at < b.updated_at) return false;
    if (a.created_at > b.created_at) return true;
    if (a.created_at < b.created_at) return false;
    return a.title < b.title;
  });

  nlohmann::json data = nlohmann::json::array();
  const size_t out_count = std::min(static_cast<size_t>(limit), visible.size());
  for (size_t i = 0; i < out_count; ++i) {
    const auto& card = visible[i];
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
    item["deleted_at"] = nlohmann::json(nullptr);
    data.push_back(std::move(item));
  }

  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = std::move(data);
  return support::json_response(http::status::ok, payload);
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
  if (path == "/cards/context" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    const std::string parent_raw = param_get("parent_card_id");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      return true;
    }

    try {
      holder::store::ProjectRepo project_repo(db);
      const auto project_opt = project_repo.get(project_id);
      if (!project_opt.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "Project not found.");
        return true;
      }

      holder::store::CardRepo card_repo(db);
      const auto all_cards = card_repo.list_all(project_id);
      std::unordered_map<std::string, holder::model::Card> cards_by_id;
      cards_by_id.reserve(all_cards.size());
      for (const auto& c : all_cards) {
        if (!c.deleted_at.has_value()) {
          cards_by_id[c.card_id] = c;
        }
      }

      std::optional<std::string> parent_card_id = std::nullopt;
      if (!parent_raw.empty()) {
        parent_card_id = normalize_parent_id(std::optional<std::string>(parent_raw));
      }

      if (parent_card_id.has_value()) {
        const auto parent_it = cards_by_id.find(parent_card_id.value());
        if (parent_it == cards_by_id.end()) {
          res = support::error_response(http::status::not_found, "not_found", "Parent card not found.");
          return true;
        }
      }

      std::unordered_map<std::string, int> child_count_by_parent;
      for (const auto& [card_id, card] : cards_by_id) {
        const auto parent = normalize_parent_id(card.parent_card_id);
        if (parent.has_value() && cards_by_id.find(parent.value()) != cards_by_id.end()) {
          child_count_by_parent[parent.value()] += 1;
        }
      }

      std::vector<holder::model::Card> level_cards;
      if (!parent_card_id.has_value()) {
        level_cards = card_repo.list_roots(project_id);
      } else {
        level_cards = card_repo.list_children(project_id, parent_card_id.value());
      }

      nlohmann::json cards_json = nlohmann::json::array();
      for (const auto& card : level_cards) {
        if (card.deleted_at.has_value()) {
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
        item["deleted_at"] = nlohmann::json(nullptr);
        const auto child_it = child_count_by_parent.find(card.card_id);
        item["child_count"] = child_it == child_count_by_parent.end() ? 0 : child_it->second;
        cards_json.push_back(std::move(item));
      }

      nlohmann::json breadcrumbs = nlohmann::json::array();
      breadcrumbs.push_back({
          {"type", "project"},
          {"project_id", project_opt->project_id},
          {"title", project_opt->name},
      });

      if (parent_card_id.has_value()) {
        std::vector<holder::model::Card> chain;
        std::optional<std::string> cursor = parent_card_id;
        int guard = 0;
        while (cursor.has_value() && guard < 1024) {
          const auto it = cards_by_id.find(cursor.value());
          if (it == cards_by_id.end()) {
            break;
          }
          chain.push_back(it->second);
          cursor = normalize_parent_id(it->second.parent_card_id);
          guard++;
        }
        std::reverse(chain.begin(), chain.end());
        for (const auto& card : chain) {
          breadcrumbs.push_back({
              {"type", "card"},
              {"card_id", card.card_id},
              {"title", card.title},
          });
        }
      }

      nlohmann::json data;
      data["project"] = {
          {"project_id", project_opt->project_id},
          {"name", project_opt->name},
      };
      data["current_parent_card_id"] = parent_card_id.has_value()
                                           ? nlohmann::json(parent_card_id.value())
                                           : nlohmann::json(nullptr);
      data["breadcrumbs"] = std::move(breadcrumbs);
      data["cards"] = std::move(cards_json);

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = std::move(data);
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path == "/cards/overview" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    const std::string limit_raw = param_get("limit");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      return true;
    }

    const auto limit = parse_limit_param(limit_raw, res);
    if (!limit.has_value()) return true;

    try {
      res = recent_cards_response(db, project_id, limit.value());
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::internal_server_error, "error", ex.what());
    }
    return true;
  }

  if (path == "/cards/recent" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    const std::string limit_raw = param_get("limit");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      return true;
    }

    const auto limit = parse_limit_param(limit_raw, res);
    if (!limit.has_value()) return true;

    try {
      res = recent_cards_response(db, project_id, limit.value());
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::internal_server_error, "error", ex.what());
    }
    return true;
  }

  if (path == "/cards" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    const std::string parent_raw = param_get("parent_card_id");
    const std::string scope_raw = param_get("scope");
    const std::string include_deleted_raw = param_get("include_deleted");
    if (project_id.empty()) {
      res = support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
    } else {
      try {
        holder::store::CardRepo repo(db);
        const std::string scope = scope_raw.empty() ? "root" : scope_raw;

        std::vector<holder::model::Card> cards;
        if (scope == "root") {
          if (!parent_raw.empty()) {
            res = support::error_response(http::status::bad_request,
                                          "bad_request",
                                          "parent_card_id is not allowed when scope=root.");
            return true;
          }
          cards = repo.list_roots(project_id);
        } else if (scope == "children") {
          if (parent_raw.empty()) {
            res = support::error_response(http::status::bad_request,
                                          "bad_request",
                                          "parent_card_id is required when scope=children.");
            return true;
          }
          cards = repo.list_children(project_id, parent_raw);
        } else if (scope == "all") {
          if (!parent_raw.empty()) {
            res = support::error_response(http::status::bad_request,
                                          "bad_request",
                                          "parent_card_id is not allowed when scope=all.");
            return true;
          }
          cards = repo.list_all(project_id);
        } else {
          res = support::error_response(http::status::bad_request,
                                        "bad_request",
                                        "Invalid scope. Expected root, children, or all.");
          return true;
        }

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
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::internal_server_error, "error", ex.what());
      }
    }
    return true;
  }

  if (path == "/cards" && req.method() == http::verb::post) {
    if (!card_store) {
      res = support::error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
    } else {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("project_id") || !body.contains("title") || !body.contains("content")) {
          res = support::error_response(http::status::bad_request, "bad_request", "Missing required fields.");
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
            card.created_at = support::now_epoch_seconds();
          }
          if (card.updated_at <= 0) {
            card.updated_at = card.created_at;
          }
          if (body.contains("parent_card_id") && !body.at("parent_card_id").is_null()) {
            card.parent_card_id = body.at("parent_card_id").get<std::string>();
          }
          const bool has_sort_key = body.contains("sort_key");
          if (has_sort_key) {
            card.sort_key = body.at("sort_key").get<double>();
          }
          if (body.contains("rel_path") && !body.at("rel_path").is_null()) {
            card.rel_path = body.at("rel_path").get<std::string>();
          }

          const std::string content = body.at("content").get<std::string>();
          if (card.rel_path.empty()) {
            card.rel_path = holder::core::card_rel_path(card.card_id);
          }
          std::optional<double> explicit_sort_key;
          if (has_sort_key) {
            explicit_sort_key = card.sort_key;
          }
          card_store->create(card, content, explicit_sort_key);

          nlohmann::json data;
          data["card_id"] = card.card_id;
          data["rel_path"] = card.rel_path;
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = support::json_response(http::status::created, payload);
        }
      } catch (const holder::privacy::PrivacyError& ex) {
        res = privacy_error_response(ex);
      } catch (const std::exception& ex) {
        const std::string msg = ex.what();
        if (msg.rfind("conflict:", 0) == 0) {
          res = support::error_response(http::status::conflict, "conflict", msg);
        } else {
          res = support::error_response(http::status::bad_request, "bad_request", msg);
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
        res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (!card_store) {
        res = support::error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
      } else if (tail == "/move") {
        if (req.method() != http::verb::post) {
          res = support::error_response(http::status::method_not_allowed,
                                        "method_not_allowed",
                                        "Method not allowed.");
        } else {
          try {
            const auto body = nlohmann::json::parse(req.body());
            if (!body.contains("project_id") || !body.contains("intent")) {
              res = support::error_response(http::status::bad_request,
                                            "bad_request",
                                            "Missing required fields.");
              return true;
            }

            const std::string project_id = body.at("project_id").get<std::string>();
            const std::string intent = body.at("intent").get<std::string>();

            const auto source_opt = card_store->get(card_id);
            if (!source_opt.has_value() || source_opt->deleted_at.has_value()) {
              res = support::error_response(http::status::not_found, "not_found", "Card not found.");
              return true;
            }
            const auto source = source_opt.value();
            if (source.project_id != project_id) {
              res = support::error_response(http::status::unprocessable_entity,
                                            "cross_project_move_forbidden",
                                            "Source card is in a different project.");
              return true;
            }

            holder::store::CardRepo card_repo(db);
            const auto cards = card_repo.list_all(project_id);
            std::unordered_map<std::string, holder::model::Card> cards_by_id;
            cards_by_id.reserve(cards.size());
            for (const auto& c : cards) {
              cards_by_id[c.card_id] = c;
            }

            auto siblings_for_parent = [&](const std::optional<std::string>& parent,
                                           const std::string& exclude_card_id) {
              std::vector<holder::model::Card> siblings;
              for (const auto& c : cards) {
                if (c.deleted_at.has_value()) {
                  continue;
                }
                if (c.card_id == exclude_card_id) {
                  continue;
                }
                if (normalize_parent_id(c.parent_card_id) == normalize_parent_id(parent)) {
                  siblings.push_back(c);
                }
              }
              std::sort(siblings.begin(), siblings.end(), [](const auto& a, const auto& b) {
                if (a.sort_key < b.sort_key) return true;
                if (a.sort_key > b.sort_key) return false;
                if (a.updated_at > b.updated_at) return true;
                if (a.updated_at < b.updated_at) return false;
                return a.title < b.title;
              });
              return siblings;
            };

            std::optional<std::string> next_parent = normalize_parent_id(source.parent_card_id);
            std::optional<double> next_sort_key;
            std::optional<std::string> moved_into_title;

            if (intent == "into" || intent == "before" || intent == "after") {
              if (!body.contains("target_card_id") || body.at("target_card_id").is_null()) {
                res = support::error_response(http::status::bad_request,
                                              "missing_target_card_id",
                                              "target_card_id is required for this intent.");
                return true;
              }
              const std::string target_card_id = body.at("target_card_id").get<std::string>();
              const auto it = cards_by_id.find(target_card_id);
              if (it == cards_by_id.end() || it->second.deleted_at.has_value()) {
                res = support::error_response(http::status::not_found, "target_not_found", "Target card not found.");
                return true;
              }
              const auto& target = it->second;
              if (target.project_id != project_id) {
                res = support::error_response(http::status::unprocessable_entity,
                                              "cross_project_move_forbidden",
                                              "Target card is in a different project.");
                return true;
              }

              if (intent == "into") {
                next_parent = target.card_id;
                if (is_descendant_of(cards_by_id, next_parent, source.card_id)) {
                  res = support::error_response(http::status::unprocessable_entity,
                                                "move_would_create_cycle",
                                                "Move would create a cycle.");
                  return true;
                }
                next_sort_key = card_repo.next_sort_key(project_id, next_parent);
                moved_into_title = target.title;
              } else {
                next_parent = normalize_parent_id(target.parent_card_id);
                const auto siblings = siblings_for_parent(next_parent, source.card_id);
                next_sort_key = sort_key_around_target(siblings, target.card_id, intent == "after");
              }
            } else if (intent == "to_start" || intent == "to_end") {
              if (!body.contains("parent_card_id")) {
                res = support::error_response(http::status::bad_request,
                                              "missing_parent_card_id",
                                              "parent_card_id is required for this intent.");
                return true;
              }
              next_parent = normalize_parent_id(body.at("parent_card_id"));
              if (next_parent.has_value()) {
                const auto parent_it = cards_by_id.find(next_parent.value());
                if (parent_it == cards_by_id.end() || parent_it->second.deleted_at.has_value()) {
                  res = support::error_response(http::status::not_found,
                                                "target_not_found",
                                                "Parent card not found.");
                  return true;
                }
              }
              const auto siblings = siblings_for_parent(next_parent, source.card_id);
              if (siblings.empty()) {
                next_sort_key = 0.0;
              } else if (intent == "to_start") {
                next_sort_key = siblings.front().sort_key - 1.0;
              } else {
                next_sort_key = siblings.back().sort_key + 1.0;
              }
            } else if (intent == "up_level") {
              const auto current_parent = normalize_parent_id(source.parent_card_id);
              if (!current_parent.has_value()) {
                res = support::error_response(http::status::unprocessable_entity,
                                              "invalid_move_intent",
                                              "Card is already at project root.");
                return true;
              }
              const auto parent_it = cards_by_id.find(current_parent.value());
              if (parent_it == cards_by_id.end() || parent_it->second.deleted_at.has_value()) {
                next_parent = std::nullopt;
              } else {
                next_parent = normalize_parent_id(parent_it->second.parent_card_id);
              }
              next_sort_key = card_repo.next_sort_key(project_id, next_parent);
              if (next_parent.has_value()) {
                const auto dest_parent_it = cards_by_id.find(next_parent.value());
                if (dest_parent_it != cards_by_id.end() && !dest_parent_it->second.deleted_at.has_value()) {
                  moved_into_title = dest_parent_it->second.title;
                }
              }
            } else {
              res = support::error_response(http::status::bad_request,
                                            "invalid_move_intent",
                                            "Unknown move intent.");
              return true;
            }

            if (!next_sort_key.has_value()) {
              next_sort_key = card_repo.next_sort_key(project_id, next_parent);
            }
            const long long updated_at = support::now_epoch_seconds();
            card_store->move(card_id, true, next_parent, next_sort_key, updated_at);
            const auto moved_opt = card_store->get(card_id);
            if (!moved_opt.has_value()) {
              res = support::error_response(http::status::not_found, "not_found", "Card not found.");
              return true;
            }

            nlohmann::json data;
            data["card_id"] = moved_opt->card_id;
            data["parent_card_id"] = moved_opt->parent_card_id.has_value()
                                         ? nlohmann::json(moved_opt->parent_card_id.value())
                                         : nlohmann::json(nullptr);
            data["sort_key"] = moved_opt->sort_key;
            data["revision"] = moved_opt->updated_at;
            data["moved_into_title"] = moved_into_title.has_value()
                                           ? nlohmann::json(moved_into_title.value())
                                           : nlohmann::json(nullptr);

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = std::move(data);
            res = support::json_response(http::status::ok, payload);
          } catch (const holder::privacy::PrivacyError& ex) {
            res = privacy_error_response(ex);
          } catch (const std::runtime_error& ex) {
            const std::string msg = ex.what();
            if (msg == "invalid_target") {
              res = support::error_response(http::status::unprocessable_entity,
                                            "invalid_target",
                                            "Target card is invalid for requested move.");
            } else {
              res = support::error_response(http::status::bad_request, "bad_request", msg);
            }
          } catch (const std::exception& ex) {
            res = support::error_response(http::status::bad_request, "bad_request", ex.what());
          }
        }
      } else if (tail == "/links") {
        try {
          const auto card_opt = card_store->get(card_id);
          if (!card_opt.has_value()) {
            res = support::error_response(http::status::not_found, "not_found", "Card not found.");
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
              res = support::json_response(http::status::ok, payload);
            } else if (req.method() == http::verb::post) {
              const auto body = nlohmann::json::parse(req.body());
              if (!body.contains("to_card_id")) {
                res = support::error_response(http::status::bad_request, "bad_request", "Missing to_card_id.");
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
                  link.created_at = support::now_epoch_seconds();
                }
                std::string validation_error;
                if (!validate_link_target(db,
                                          card.project_id,
                                          link.to_card_id,
                                          link.to_type,
                                          validation_error)) {
                  res = support::error_response(http::status::bad_request, "bad_request", validation_error);
                } else {
                  repo.upsert_links(card.project_id, card.card_id, {link});
                  card_store->update_links(card.card_id, support::now_epoch_seconds());

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
                repo.delete_link(card.project_id, card.card_id, to_card_id.value(), to_type, kind);
              } else {
                repo.delete_links_from(card.project_id, card.card_id);
              }
              card_store->update_links(card.card_id, support::now_epoch_seconds());

              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"card_id", card.card_id}};
              res = support::json_response(http::status::ok, payload);
            } else {
              res = support::error_response(http::status::method_not_allowed,
                                   "method_not_allowed",
                                   "Method not allowed.");
            }
          }
        } catch (const holder::privacy::PrivacyError& ex) {
          res = privacy_error_response(ex);
        } catch (const std::exception& ex) {
          res = support::error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (tail == "/backlinks") {
        try {
          const auto card_opt = card_store->get(card_id);
          if (!card_opt.has_value()) {
            res = support::error_response(http::status::not_found, "not_found", "Card not found.");
          } else if (req.method() != http::verb::get) {
            res = support::error_response(http::status::method_not_allowed,
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
            res = support::json_response(http::status::ok, payload);
          }
        } catch (const holder::privacy::PrivacyError& ex) {
          res = privacy_error_response(ex);
        } catch (const std::exception& ex) {
          res = support::error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (tail == "/restore") {
        if (req.method() != http::verb::post) {
          res = support::error_response(http::status::method_not_allowed,
                               "method_not_allowed",
                               "Method not allowed.");
        } else {
          try {
            const long long updated_at = support::now_epoch_seconds();
            card_store->restore(card_id, updated_at);
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"card_id", card_id}};
            res = support::json_response(http::status::ok, payload);
          } catch (const holder::privacy::PrivacyError& ex) {
            res = privacy_error_response(ex);
          } catch (const std::exception& ex) {
            res = support::error_response(http::status::bad_request, "bad_request", ex.what());
          }
        }
      } else {
        res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      }
    } else {
      const std::string card_id = rest;
      if (card_id.empty()) {
        res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (!card_store) {
        res = support::error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
      } else if (req.method() == http::verb::get) {
        try {
          const auto card_opt = card_store->get(card_id);
          if (!card_opt.has_value()) {
            res = support::error_response(http::status::not_found, "not_found", "Card not found.");
          } else {
            const auto& card = card_opt.value();
            if (card.deleted_at.has_value()) {
              res = support::error_response(http::status::not_found, "not_found", "Card not found.");
            } else {
              const auto content_opt = card_store->get_content(card);
              if (!content_opt.has_value()) {
                res = support::error_response(http::status::not_found, "not_found", "Card content missing.");
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
                res = support::json_response(http::status::ok, payload);
              }
            }
          }
        } catch (const std::exception& ex) {
          res = support::error_response(http::status::internal_server_error, "error", ex.what());
        }
      } else if (req.method() == http::verb::patch) {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("updated_at")) {
            res = support::error_response(http::status::bad_request, "bad_request", "Missing required fields.");
          } else {
            const bool has_content = body.contains("content");
            const bool has_title = body.contains("title");
            const bool has_parent_card_id = body.contains("parent_card_id");
            const bool has_sort_key = body.contains("sort_key");
            if (!has_content && !has_title && !has_parent_card_id && !has_sort_key) {
              res = support::error_response(http::status::bad_request,
                                            "bad_request",
                                            "No updatable fields supplied.");
              return true;
            }

            std::optional<std::string> title;
            if (has_title && !body.at("title").is_null()) {
              title = body.at("title").get<std::string>();
            }
            const long long updated_at = body.at("updated_at").get<long long>();

            if (has_content || has_title) {
              if (!has_content) {
                res = support::error_response(http::status::bad_request,
                                              "bad_request",
                                              "content is required when updating title.");
                return true;
              }
              const std::string content = body.at("content").get<std::string>();
              card_store->update_content(card_id, content, title, updated_at);
            }

            if (has_parent_card_id || has_sort_key) {
              std::optional<std::string> parent_card_id;
              if (has_parent_card_id && !body.at("parent_card_id").is_null()) {
                parent_card_id = body.at("parent_card_id").get<std::string>();
              }
              std::optional<double> sort_key;
              if (has_sort_key) {
                sort_key = body.at("sort_key").get<double>();
              }
              card_store->move(card_id, has_parent_card_id, parent_card_id, sort_key, updated_at);
            }

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"card_id", card_id}};
            res = support::json_response(http::status::ok, payload);
          }
        } catch (const holder::privacy::PrivacyError& ex) {
          res = privacy_error_response(ex);
        } catch (const std::exception& ex) {
          res = support::error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::delete_) {
        try {
          const long long deleted_at = support::now_epoch_seconds();
          card_store->trash(card_id, deleted_at);
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"card_id", card_id}};
          res = support::json_response(http::status::ok, payload);
        } catch (const holder::privacy::PrivacyError& ex) {
          res = privacy_error_response(ex);
        } catch (const std::exception& ex) {
          res = support::error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::post && rest.size() > 0) {
        res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      } else {
        res = support::error_response(http::status::not_found, "not_found", "Route not found.");
      }
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
