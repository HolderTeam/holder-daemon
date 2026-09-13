#include "api/routes/CardReferenceRoutes.h"

#include "api/support/HttpResponses.h"
#include "card/CardReferenceResolver.h"
#include "card/CardRepo.h"
#include "model/Card.h"
#include "project/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

struct ResolveRequest {
  std::string project_id;
  std::string reference;
  holder::model::CardScope scope;
};

std::optional<holder::model::CardScope> parse_scope(const std::string& scope) {
  if (scope == "live") return holder::model::CardScope::Live;
  if (scope == "trashed") return holder::model::CardScope::Trashed;
  if (scope == "either") return holder::model::CardScope::Either;
  return std::nullopt;
}

std::optional<ResolveRequest> parse_request(
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res
) {
  try {
    const auto body = nlohmann::json::parse(req.body());
    if (!body.is_object() || !body.contains("project_id") || !body.contains("reference") ||
        !body.contains("scope")) {
      res = support::error_response(
          http::status::bad_request,
          "bad_request",
          "Missing required fields."
      );
      return std::nullopt;
    }

    ResolveRequest parsed{
        body.at("project_id").get<std::string>(),
        body.at("reference").get<std::string>(),
        holder::model::CardScope::Live,
    };
    const auto scope = parse_scope(body.at("scope").get<std::string>());
    if (!scope.has_value()) {
      res = support::error_response(
          http::status::bad_request,
          "bad_request",
          "scope must be one of: live, trashed, either."
      );
      return std::nullopt;
    }
    if (parsed.project_id.empty() || parsed.reference.empty()) {
      res = support::error_response(
          http::status::bad_request,
          "bad_request",
          "project_id and reference must not be empty."
      );
      return std::nullopt;
    }
    parsed.scope = *scope;
    return parsed;
  } catch (const nlohmann::json::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    return std::nullopt;
  }
}

std::string match_kind_name(holder::card::CardReferenceMatchKind match_kind) {
  switch (match_kind) {
  case holder::card::CardReferenceMatchKind::FullId:
    return "full_id";
  case holder::card::CardReferenceMatchKind::IdPrefix:
    return "id_prefix";
  case holder::card::CardReferenceMatchKind::ExactTitle:
    return "exact_title";
  }
  return {}; // LCOV_EXCL_LINE
}

nlohmann::json card_summary(const holder::model::Card& card) {
  return {
      {"card_id", card.card_id},
      {"title", card.title},
      {"deleted_at",
       card.deleted_at.has_value() ? nlohmann::json(*card.deleted_at) : nlohmann::json(nullptr)},
  };
}

nlohmann::json result_data(const holder::card::CardReferenceResult& result) {
  nlohmann::json data;
  switch (result.status) {
  case holder::card::CardReferenceStatus::Resolved:
    data["status"] = "resolved";
    data["match_kind"] = match_kind_name(*result.match_kind);
    data["card"] = card_summary(*result.card);
    break;
  case holder::card::CardReferenceStatus::Ambiguous:
    data["status"] = "ambiguous";
    data["match_kind"] = match_kind_name(*result.match_kind);
    data["candidates"] = nlohmann::json::array();
    for (const auto& candidate : result.candidates) {
      data["candidates"].push_back(card_summary(candidate));
    }
    break;
  case holder::card::CardReferenceStatus::NotFound:
    data["status"] = "not_found";
    break;
  }
  return data;
}

} // namespace

bool handle_card_reference_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db
) {
  if (path != "/card-references/resolve" || req.method() != http::verb::post) {
    return false;
  }

  const auto parsed = parse_request(req, res);
  if (!parsed.has_value()) return true;

  try {
    holder::project::ProjectRepo projects(db);
    if (!projects.get(parsed->project_id).has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "Project not found.");
      return true;
    }

    holder::card::CardRepo cards(db);
    holder::card::CardReferenceResolver resolver(cards);
    const auto result = resolver.resolve(parsed->project_id, parsed->reference, parsed->scope);
    res = support::json_response(
        http::status::ok,
        nlohmann::json{{"ok", true}, {"data", result_data(result)}}
    );
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::internal_server_error, "error", ex.what());
  }

  return true;
}

} // namespace holder::api::routes
