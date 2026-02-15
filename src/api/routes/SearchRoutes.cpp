#include "api/routes/SearchRoutes.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

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

} // namespace

bool handle_search_routes(const std::string& path,
                          const http::request<http::string_body>& req,
                          http::response<http::string_body>& res,
                          holder::index::FtsIndexer* fts,
                          const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/search/cards" && req.method() == http::verb::get) {
    if (!fts) {
      res = error_response(http::status::not_implemented, "not_implemented", "Search unavailable.");
      return true;
    }

    const std::string project_id = param_get("project_id");
    const std::string q = param_get("q");
    int limit = 20;
    int offset = 0;
    bool bad_params = false;
    try {
      if (!param_get("limit").empty()) limit = std::stoi(param_get("limit"));
      if (!param_get("offset").empty()) offset = std::stoi(param_get("offset"));
    } catch (const std::exception&) {
      res = error_response(http::status::bad_request, "bad_request", "Invalid limit/offset.");
      bad_params = true;
    }

    if (!bad_params) {
      if (project_id.empty() || q.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing project_id or q.");
      } else {
        try {
          const auto rows = fts->search_cards(project_id, q, limit, offset);
          nlohmann::json data = nlohmann::json::array();
          for (const auto& row : rows) {
            data.push_back({
                {"card_id", row.id},
                {"title", row.title},
                {"updated_at", row.updated_at},
                {"created_at", row.created_at},
                {"snippet", row.snippet},
                {"rank", row.rank},
            });
          }
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = json_response(http::status::ok, payload);
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    }
    return true;
  }

  if (path == "/search/ai" && req.method() == http::verb::get) {
    if (!fts) {
      res = error_response(http::status::not_implemented, "not_implemented", "Search unavailable.");
      return true;
    }

    const std::string project_id = param_get("project_id");
    const std::string q = param_get("q");
    int limit = 20;
    int offset = 0;
    bool bad_params = false;
    try {
      if (!param_get("limit").empty()) limit = std::stoi(param_get("limit"));
      if (!param_get("offset").empty()) offset = std::stoi(param_get("offset"));
    } catch (const std::exception&) {
      res = error_response(http::status::bad_request, "bad_request", "Invalid limit/offset.");
      bad_params = true;
    }

    if (!bad_params) {
      if (project_id.empty() || q.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing project_id or q.");
      } else {
        try {
          const auto rows = fts->search_messages(project_id, q, limit, offset);
          nlohmann::json data = nlohmann::json::array();
          for (const auto& row : rows) {
            data.push_back({
                {"message_id", row.id},
                {"created_at", row.created_at},
                {"snippet", row.snippet},
                {"rank", row.rank},
            });
          }
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = json_response(http::status::ok, payload);
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
