#include "api/routes/HistoryRoutes.h"

#include "api/support/HttpResponses.h"

#include "history/CardHistory.h"
#include "privacy/PrivacyError.h"
#include "project/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

struct HistoryPath {
  std::string project_id;
  std::string card_id;
  bool compare = false;
};

std::optional<HistoryPath> parse_history_path(const std::string& path) {
  static const std::string projects = "/projects/";
  static const std::string history = "/history/cards/";
  if (path.rfind(projects, 0) != 0) return std::nullopt;
  const auto project_end = path.find('/', projects.size());
  if (project_end == std::string::npos ||
      path.compare(project_end, history.size(), history) != 0) return std::nullopt;
  HistoryPath parsed;
  parsed.project_id = path.substr(projects.size(), project_end - projects.size());
  const auto card_start = project_end + history.size();
  const auto suffix = path.find('/', card_start);
  parsed.card_id = path.substr(card_start, suffix - card_start);
  if (parsed.project_id.empty() || parsed.card_id.empty()) return std::nullopt;
  if (suffix == std::string::npos) return parsed;
  if (path.substr(suffix) != "/compare") return std::nullopt;
  parsed.compare = true;
  return parsed;
}

nlohmann::json entry_json(const holder::history::CardHistoryEntry& entry) {
  return {
      {"first_oid", entry.first_oid},
      {"last_oid", entry.last_oid},
      {"parent_oids", entry.parent_oids},
      {"author", {{"name", entry.author_name}, {"email", entry.author_email}}},
      {"started_at", entry.started_at},
      {"ended_at", entry.ended_at},
      {"kind", entry.kind},
      {"summary", entry.summary},
      {"commit_count", entry.commit_count},
      {"is_merge", entry.is_merge},
  };
}

nlohmann::json version_json(const holder::history::CardVersion& version) {
  return {
      {"exists", version.exists},
      {"oid", version.oid},
      {"title", version.title},
      {"body", version.body},
  };
}

std::size_t history_limit(const std::string& raw) {
  if (raw.empty()) return 50;
  try {
    std::size_t consumed = 0;
    const auto value = std::stoul(raw, &consumed);
    if (consumed == raw.size() && value > 0 && value <= 200) return value;
  } catch (const std::exception&) {
    // Normalize all numeric parsing failures into the public validation error below.
  }
  throw std::invalid_argument("limit must be between 1 and 200");
}

bool valid_oid(const std::string& value) {
  return value.size() == 40 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

} // namespace

bool handle_history_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    const std::function<std::string(const std::string&)>& param_get
) {
  const auto parsed = parse_history_path(path);
  if (!parsed.has_value() || req.method() != http::verb::get) return false;

  try {
    const auto project = holder::project::ProjectRepo(db).get(parsed->project_id);
    if (!project.has_value()) {
      res = support::error_response(http::status::not_found, "not_found", "Project not found.");
      return true;
    }

    holder::history::CardHistoryService history;
    if (!parsed->compare) {
      const auto cursor_text = param_get("cursor");
      if (!cursor_text.empty() && !valid_oid(cursor_text)) {
        throw std::invalid_argument("cursor must be a full commit OID");
      }
      const auto cursor = cursor_text.empty()
          ? std::optional<std::string>{}
          : std::optional<std::string>{cursor_text};
      const auto page = history.list(
          *project, parsed->card_id, history_limit(param_get("limit")), cursor
      );
      nlohmann::json entries = nlohmann::json::array();
      for (const auto& entry : page.entries) entries.push_back(entry_json(entry));
      res = support::json_response(
          http::status::ok,
          {{"ok", true},
           {"data",
            {{"head_oid", page.head_oid.has_value() ? nlohmann::json(*page.head_oid)
                                                      : nlohmann::json(nullptr)},
             {"entries", std::move(entries)},
             {"next_cursor", page.next_cursor.has_value() ? nlohmann::json(*page.next_cursor)
                                                            : nlohmann::json(nullptr)}}}}
      );
      return true;
    }

    const auto from_text = param_get("from");
    const auto to_text = param_get("to");
    if (from_text.empty() || to_text.empty()) {
      res = support::error_response(
          http::status::bad_request, "bad_request", "from and to commit OIDs are required."
      );
      return true;
    }
    if (!valid_oid(from_text) || !valid_oid(to_text)) {
      res = support::error_response(
          http::status::bad_request, "bad_request", "from and to must be full commit OIDs."
      );
      return true;
    }
    const auto mode = param_get("mode");
    if (!mode.empty() && mode != "since") {
      res = support::error_response(
          http::status::bad_request, "bad_request", "Only mode=since is currently supported."
      );
      return true;
    }
    const auto comparison = history.compare(
        *project,
        parsed->card_id,
        std::optional<std::string>{from_text},
        std::optional<std::string>{to_text}
    );
    nlohmann::json lines = nlohmann::json::array();
    for (const auto& line : comparison.lines) {
      lines.push_back({
          {"origin", std::string(1, line.origin)},
          {"text", line.text},
          {"old_line", line.old_line < 0 ? nlohmann::json(nullptr) : nlohmann::json(line.old_line)},
          {"new_line", line.new_line < 0 ? nlohmann::json(nullptr) : nlohmann::json(line.new_line)},
      });
    }
    res = support::json_response(
        http::status::ok,
        {{"ok", true},
         {"data",
          {{"from", version_json(comparison.from)},
           {"to", version_json(comparison.to)},
           {"summary", comparison.summary},
           {"lines", std::move(lines)},
           {"truncated", comparison.truncated}}}}
    );
  } catch (const std::invalid_argument& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  } catch (const holder::privacy::PrivacyError& ex) {
    if (ex.code() == holder::privacy::PrivacyErrorCode::KeyMaterialMissing ||
        ex.code() == holder::privacy::PrivacyErrorCode::KeyringUnavailable) {
      res = support::error_response(http::status::conflict, "history_key_unavailable", ex.what());
    } else {
      res = support::error_response(
          http::status::service_unavailable, "history_unavailable", ex.what()
      );
    }
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::service_unavailable, "history_unavailable", ex.what());
  }
  return true;
}

} // namespace holder::api::routes
