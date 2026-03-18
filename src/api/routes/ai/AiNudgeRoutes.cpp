#include "api/routes/ai/AiNudgeRoutes.h"

#include "api/support/HttpResponses.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

struct NudgeDecision {
  bool accepted{false};
  bool should_nudge{false};
  std::string reason;
};

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool is_placeholder_title(const std::string& title) {
  return lower_copy(title).rfind("untitled", 0) == 0;
}

bool is_successful_push_status(const std::string& status) {
  return status == "pushed" || status == "up_to_date";
}

NudgeDecision evaluate_candidate(const std::string& kind, const nlohmann::json& facts) {
  if (kind == "card.title_only") {
    const auto title = facts.value("title", "");
    const auto body_empty = facts.value("body_empty", false);
    const auto doc_chars = facts.value("doc_chars", 0);
    const auto should_nudge = body_empty && !title.empty() && !is_placeholder_title(title) && doc_chars <= 160;
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "title_only_candidate_ready" : "title_only_not_actionable"};
  } // LCOV_EXCL_LINE: closing brace coverage artifact after aggregate return
  if (kind == "card.stuck_drafting") {
    const auto autosave_count = facts.value("autosave_count", 0);
    const auto body_chars = facts.value("body_chars", 0);
    const auto should_nudge = autosave_count >= 3 && body_chars > 0 && body_chars <= 160;
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "stuck_drafting_candidate_ready" : "stuck_drafting_not_actionable"};
  }
  if (kind == "git.push_failed_repeated") {
    const auto failure_count = facts.value("failure_count", 0);
    const auto latest_status = facts.value("latest_status", "");
    const auto should_nudge = failure_count >= 2 && !is_successful_push_status(latest_status);
    return {.accepted = true,
            .should_nudge = should_nudge,
            .reason = should_nudge ? "git_push_failure_candidate_ready" : "git_push_failure_not_actionable"};
  } // LCOV_EXCL_LINE: closing brace coverage artifact after aggregate return
  return {.accepted = false, .should_nudge = false, .reason = "unknown_candidate_kind"};
}

} // namespace

bool handle_ai_nudge_routes(const std::string& path,
                            const http::request<http::string_body>& req,
                            http::response<http::string_body>& res) {
  if (path != "/ai/nudges/evaluate" || req.method() != http::verb::post) {
    return false;
  }

  nlohmann::json body;
  try {
    body = nlohmann::json::parse(req.body());
  } catch (const std::exception& e) {
    res = support::error_response(http::status::bad_request, "invalid_json", e.what());
    return true;
  }

  if (!body.is_object() || !body.contains("kind") || !body["kind"].is_string() || !body.contains("project_id") ||
      !body["project_id"].is_string() || !body.contains("created_at") || !body["created_at"].is_number_integer() ||
      !body.contains("facts") || !body["facts"].is_object()) {
    res = support::error_response(http::status::bad_request,
                                  "invalid_body",
                                  "Expected kind, project_id, created_at, and facts.");
    return true;
  }

  const auto kind = body.value("kind", "");
  const auto project_id = body.value("project_id", "");
  if (kind.empty() || project_id.empty()) {
    res = support::error_response(http::status::bad_request, "invalid_body", "kind and project_id are required.");
    return true;
  }

  const auto decision = evaluate_candidate(kind, body["facts"]);

  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = {
      {"kind", kind},
      {"accepted", decision.accepted},
      {"should_nudge", decision.should_nudge},
      {"reason", decision.reason},
  };
  res = support::json_response(http::status::ok, payload);
  return true;
}

} // namespace holder::api::routes
