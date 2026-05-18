#include "api/routes/ai/AiNudgeRoutes.h"

#include "api/support/HttpQuery.h"
#include "api/support/HttpResponses.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

nlohmann::json nudge_to_json(const holder::ai::Nudge& nudge) {
  auto item = nlohmann::json{
      {"nudge_id", nudge.nudge_id},
      {"kind", nudge.kind},
      {"project_id", nudge.project_id},
      {"card_id",
       nudge.card_id.has_value() ? nlohmann::json(nudge.card_id.value()) : nlohmann::json(nullptr)},
      {"title", nudge.title},
      {"body", nudge.body},
      {"meta_json", nudge.meta_json},
      {"basis_fingerprint",
       nudge.basis_fingerprint.has_value() ? nlohmann::json(nudge.basis_fingerprint.value())
                                           : nlohmann::json(nullptr)},
      {"basis_commit",
       nudge.basis_commit.has_value() ? nlohmann::json(nudge.basis_commit.value())
                                      : nlohmann::json(nullptr)},
      {"created_at", nudge.created_at},
  };
  if (nudge.meta_json.is_object() && nudge.meta_json.contains("suggestions")) {
    item["suggestions"] = nudge.meta_json["suggestions"];
  }
  return item;
}

bool handle_nudge_list_route(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::ai::NudgeService* nudge_service
) {
  if (path != "/ai/nudges" || req.method() != http::verb::get) {
    return false;
  }
  if (nudge_service == nullptr) {
    res = support::error_response(
        http::status::internal_server_error,
        "service_unavailable",
        "Nudge service unavailable."
    );
    return true;
  }

  const auto target = std::string(req.target());
  const auto query_pos = target.find('?');
  const auto query_string = query_pos == std::string::npos ? std::string()
                                                           : target.substr(query_pos + 1);
  const auto project_id = support::query_param_value(query_string, "project_id");
  const auto card_id_raw = support::query_param_value(query_string, "card_id");
  if (project_id.empty()) {
    res = support::error_response(
        http::status::bad_request,
        "invalid_query",
        "project_id is required."
    );
    return true;
  }

  const auto nudges = nudge_service->list(
      project_id,
      card_id_raw.empty() ? std::optional<std::string>() : std::optional<std::string>(card_id_raw)
  );

  nlohmann::json items = nlohmann::json::array();
  for (const auto& nudge : nudges) {
    items.push_back(nudge_to_json(nudge));
  }

  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = {{"nudges", items}};
  res = support::json_response(http::status::ok, payload);
  return true;
}

bool handle_nudge_dismiss_route(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::ai::NudgeService* nudge_service
) {
  static constexpr std::string_view prefix = "/ai/nudges/";
  static constexpr std::string_view suffix = "/dismiss";
  if (!path.starts_with(prefix) || !path.ends_with(suffix) || req.method() != http::verb::post) {
    return false;
  }
  if (nudge_service == nullptr) {
    res = support::error_response(
        http::status::internal_server_error,
        "service_unavailable",
        "Nudge service unavailable."
    );
    return true;
  }

  const auto nudge_id = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
  if (nudge_id.empty()) {
    res = support::error_response(http::status::not_found, "not_found", "Nudge not found.");
    return true;
  }
  if (!nudge_service->dismiss(nudge_id)) {
    res = support::error_response(http::status::not_found, "not_found", "Nudge not found.");
    return true;
  }

  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = {{"nudge_id", nudge_id}, {"dismissed", true}};
  res = support::json_response(http::status::ok, payload);
  return true;
}

} // namespace

bool handle_ai_nudge_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::ai::NudgeService* nudge_service
) {
  if (handle_nudge_list_route(path, req, res, nudge_service)) {
    return true;
  }
  if (handle_nudge_dismiss_route(path, req, res, nudge_service)) {
    return true;
  }
  if (path != "/ai/nudges/evaluate" || req.method() != http::verb::post) {
    return false;
  }
  if (nudge_service == nullptr) {
    res = support::error_response(
        http::status::internal_server_error,
        "service_unavailable",
        "Nudge service unavailable."
    );
    return true;
  }

  nlohmann::json body;
  try {
    body = nlohmann::json::parse(req.body());
  } catch (const std::exception& e) {
    res = support::error_response(http::status::bad_request, "invalid_json", e.what());
    return true;
  }

  if (!body.is_object() || !body.contains("kind") || !body["kind"].is_string() ||
      !body.contains("project_id") || !body["project_id"].is_string() ||
      !body.contains("created_at") || !body["created_at"].is_number_integer() ||
      !body.contains("facts") || !body["facts"].is_object()) {
    res = support::error_response(
        http::status::bad_request,
        "invalid_body",
        "Expected kind, project_id, created_at, and facts."
    );
    return true;
  }

  const auto kind = body.value("kind", "");
  const auto project_id = body.value("project_id", "");
  if (kind.empty() || project_id.empty()) {
    res = support::error_response(
        http::status::bad_request,
        "invalid_body",
        "kind and project_id are required."
    );
    return true;
  }

  holder::ai::NudgeCandidateInput input{
      .kind = kind,
      .project_id = project_id,
      .card_id = body.contains("card_id") && body["card_id"].is_string()
                     ? std::optional<std::string>(body["card_id"].get<std::string>())
                     : std::optional<std::string>(),
      .created_at = body.value("created_at", std::int64_t{0}), // LCOV_EXCL_LINE
      .basis_fingerprint =
          body.contains("basis_fingerprint") && body["basis_fingerprint"].is_string()
              ? std::optional<std::string>(body["basis_fingerprint"].get<std::string>())
              : std::optional<std::string>(),
      .basis_commit = body.contains("basis_commit") && body["basis_commit"].is_string()
                          ? std::optional<std::string>(body["basis_commit"].get<std::string>())
                          : std::optional<std::string>(),
      .facts = body["facts"],
  };
  const auto decision = nudge_service->evaluate_and_record(input);

  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = {
      {"kind", kind},
      {"accepted", decision.accepted},
      {"should_nudge", decision.should_nudge},
      {"reason", decision.reason},
      {"nudge",
       decision.nudge.has_value() ? nudge_to_json(decision.nudge.value()) : nlohmann::json(nullptr)
      },
  };
  res = support::json_response(http::status::ok, payload);
  return true;
}

} // namespace holder::api::routes
