#include "api/routes/MilestoneRoutes.h"

#include "api/support/HttpResponses.h"
#include "api/support/Time.h"

#include "card/CardRepo.h"
#include "card/MilestoneRepo.h"
#include "project/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

nlohmann::json optional_string_json(const std::optional<std::string>& value) {
  return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

nlohmann::json milestone_json(
    const holder::model::Milestone& milestone,
    const std::optional<std::string>& card_title = std::nullopt
) {
  nlohmann::json body = {
      {"milestone_id", milestone.milestone_id},
      {"card_id", milestone.card_id},
      {"start_at", milestone.start_at},
      {"end_at",
       milestone.end_at.has_value() ? nlohmann::json(*milestone.end_at) : nlohmann::json(nullptr)},
      {"all_day", milestone.all_day},
      {"kind", optional_string_json(milestone.kind)},
      {"description", optional_string_json(milestone.description)},
      {"created_at", milestone.created_at},
      {"updated_at", milestone.updated_at},
  };
  if (card_title.has_value()) body["card_title"] = *card_title;
  return body;
}

nlohmann::json card_activity_json(const holder::model::Card& card) {
  return {
      {"card_id", card.card_id},
      {"title", card.title},
      {"created_at", card.created_at},
      {"updated_at", card.updated_at},
  };
}

std::optional<long long> parse_required_epoch(
    const std::string& raw,
    const std::string& name,
    http::response<http::string_body>& res
) {
  if (raw.empty()) {
    res = support::error_response(
        http::status::bad_request, "bad_request", "Missing " + name + "."
    );
    return std::nullopt;
  }
  try {
    std::size_t consumed = 0;
    const auto value = std::stoll(raw, &consumed);
    if (consumed != raw.size()) throw std::invalid_argument("trailing characters");
    return value;
  } catch (const std::exception&) {
    res = support::error_response(
        http::status::bad_request, "bad_request", name + " must be an epoch-second integer."
    );
    return std::nullopt;
  }
}

std::optional<std::string> nullable_string(
    const nlohmann::json& body,
    const std::string& name
) {
  if (!body.contains(name) || body.at(name).is_null()) return std::nullopt;
  if (!body.at(name).is_string()) throw std::invalid_argument(name + " must be a string or null.");
  const auto value = body.at(name).get<std::string>();
  return value.empty() ? std::nullopt : std::optional<std::string>(value);
}

bool parse_card_milestone_path(
    const std::string& path,
    std::string& card_id,
    std::optional<std::string>& milestone_id
) {
  static const std::string prefix = "/cards/";
  static const std::string marker = "/milestones";
  if (path.rfind(prefix, 0) != 0) return false;
  const auto marker_pos = path.find('/', prefix.size());
  if (marker_pos == std::string::npos || path.compare(marker_pos, marker.size(), marker) != 0) {
    return false;
  }
  card_id = path.substr(prefix.size(), marker_pos - prefix.size());
  const auto suffix = path.substr(marker_pos + marker.size());
  if (suffix.empty()) {
    milestone_id.reset();
    return true;
  }
  if (suffix[0] != '/' || suffix.size() == 1 || suffix.find('/', 1) != std::string::npos) {
    return false;
  }
  milestone_id = suffix.substr(1);
  return true;
}

} // namespace

bool handle_milestone_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::card::CardStore* card_store,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get
) {
  if (path == "/calendar" && req.method() == http::verb::get) {
    const auto project_id = param_get("project_id");
    if (project_id.empty()) {
      res = support::error_response(
          http::status::bad_request, "bad_request", "Missing project_id."
      );
      return true;
    }
    const auto from = parse_required_epoch(param_get("from"), "from", res);
    if (!from.has_value()) return true;
    const auto to = parse_required_epoch(param_get("to"), "to", res);
    if (!to.has_value()) return true;
    if (*from > *to) {
      res = support::error_response(
          http::status::bad_request, "bad_request", "from must not be after to."
      );
      return true;
    }

    try {
      if (!holder::project::ProjectRepo(db).get(project_id).has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "Project not found.");
        return true;
      }
      const auto cards = holder::card::CardRepo(db).list_all(project_id);
      std::unordered_map<std::string, std::string> active_titles;
      nlohmann::json created_cards = nlohmann::json::array();
      nlohmann::json updated_cards = nlohmann::json::array();
      for (const auto& card : cards) {
        if (card.deleted_at.has_value()) continue;
        active_titles.emplace(card.card_id, card.title);
        if (card.created_at >= *from && card.created_at <= *to) {
          created_cards.push_back(card_activity_json(card));
        }
        if (card.updated_at != card.created_at && card.updated_at >= *from && card.updated_at <= *to) {
          updated_cards.push_back(card_activity_json(card));
        }
      }

      nlohmann::json milestones = nlohmann::json::array();
      for (const auto& milestone :
           holder::card::MilestoneRepo(db).list_in_range(project_id, *from, *to)) {
        const auto title = active_titles.find(milestone.card_id);
        if (title == active_titles.end()) continue;
        milestones.push_back(milestone_json(
            milestone,
            std::optional<std::string>(title->second)
        ));
      }
      res = support::json_response(
          http::status::ok,
          {{"ok", true},
           {"data",
            {{"project_id", project_id},
             {"from", *from},
             {"to", *to},
             {"milestones", std::move(milestones)},
             {"created_cards", std::move(created_cards)},
             {"updated_cards", std::move(updated_cards)}}}}
      );
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  std::string card_id;
  std::optional<std::string> milestone_id;
  if (!parse_card_milestone_path(path, card_id, milestone_id)) return false;
  if (card_id.empty()) {
    res = support::error_response(http::status::bad_request, "bad_request", "Missing card_id.");
    return true;
  }

  try {
    const auto card = holder::card::CardRepo(db).get(card_id);
    if (!card.has_value() || card->deleted_at.has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "Card not found.");
      return true;
    }
    holder::card::MilestoneRepo repo(db);

    if (!milestone_id.has_value() && req.method() == http::verb::get) {
      nlohmann::json data = nlohmann::json::array();
      for (const auto& milestone : repo.list_for_card(card->project_id, card_id)) {
        data.push_back(milestone_json(milestone));
      }
      res = support::json_response(http::status::ok, {{"ok", true}, {"data", data}});
      return true;
    }

    if (!milestone_id.has_value() && req.method() == http::verb::post) {
      if (card_store == nullptr) {
        res = support::error_response(
            http::status::not_implemented, "not_implemented", "Card store unavailable."
        );
        return true;
      }
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("start_at") || !body.at("start_at").is_number_integer()) {
        throw std::invalid_argument("start_at must be an epoch-second integer.");
      }
      holder::model::Milestone milestone;
      milestone.milestone_id = uuid_v4();
      milestone.project_id = card->project_id;
      milestone.card_id = card_id;
      milestone.start_at = body.at("start_at").get<long long>();
      if (body.contains("end_at") && !body.at("end_at").is_null()) {
        if (!body.at("end_at").is_number_integer()) {
          throw std::invalid_argument("end_at must be an epoch-second integer or null.");
        }
        milestone.end_at = body.at("end_at").get<long long>();
        if (*milestone.end_at < milestone.start_at) {
          throw std::invalid_argument("end_at must not be before start_at.");
        }
      }
      if (body.contains("all_day") && !body.at("all_day").is_boolean()) {
        throw std::invalid_argument("all_day must be a boolean.");
      }
      milestone.all_day = body.value("all_day", false);
      milestone.kind = nullable_string(body, "kind");
      milestone.description = nullable_string(body, "description");
      milestone.created_at = support::now_epoch_seconds();
      milestone.updated_at = milestone.created_at;

      auto milestones = repo.list_for_card(card->project_id, card_id);
      milestones.push_back(milestone);
      repo.replace_for_card(card->project_id, card_id, milestones);
      card_store->update_milestones(card_id, support::now_epoch_seconds());
      res = support::json_response(
          http::status::created, {{"ok", true}, {"data", milestone_json(milestone)}}
      );
      return true;
    }

    if (milestone_id.has_value() && req.method() == http::verb::delete_) {
      if (card_store == nullptr) {
        res = support::error_response(
            http::status::not_implemented, "not_implemented", "Card store unavailable."
        );
        return true;
      }
      auto milestones = repo.list_for_card(card->project_id, card_id);
      const auto before = milestones.size();
      milestones.erase(
          std::remove_if(
              milestones.begin(),
              milestones.end(),
              [&](const auto& milestone) { return milestone.milestone_id == *milestone_id; }
          ),
          milestones.end()
      );
      const bool removed = milestones.size() != before;
      if (removed) {
        repo.replace_for_card(card->project_id, card_id, milestones);
        card_store->update_milestones(card_id, support::now_epoch_seconds());
      }
      res = support::json_response(
          http::status::ok,
          {{"ok", true},
           {"data", {{"card_id", card_id}, {"milestone_id", *milestone_id}, {"removed", removed}}}}
      );
      return true;
    }
  } catch (const nlohmann::json::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    return true;
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    return true;
  }

  return false;
}

} // namespace holder::api::routes
