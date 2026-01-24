#include "api/Session.h"

#include "core/CardPaths.h"
#include "core/ServerInfo.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace holder::api {
namespace {

namespace http = boost::beast::http;

bool is_authorized(const http::request<http::string_body>& req, const std::string& token) {
  const auto it = req.find(http::field::authorization);
  if (it == req.end()) return false;

  const auto value = it->value();
  const std::string auth(value.data(), value.size());
  constexpr char kPrefix[] = "Bearer ";
  if (auth.rfind(kPrefix, 0) != 0) return false;

  const std::string bearer = auth.substr(sizeof(kPrefix) - 1);
  return bearer == token;
}

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

http::response<http::string_body> text_response(http::status status,
                                                std::string body,
                                                std::string content_type) {
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, std::move(content_type));
  res.keep_alive(false);
  res.body() = std::move(body);
  res.prepare_payload();
  return res;
}

std::optional<std::filesystem::path> find_openapi_path() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HOLDER_OPENAPI_PATH")) {
    fs::path p(env);
    if (fs::exists(p)) return p;
  }
  fs::path p1 = fs::current_path() / "openapi.yaml";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "openapi.yaml";
  if (fs::exists(p2)) return p2;
  return std::nullopt;
}

std::optional<std::filesystem::path> find_docs_root() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HOLDER_DOCS_ROOT")) {
    fs::path p(env);
    if (fs::exists(p) && fs::is_directory(p)) return p;
  }
  fs::path p1 = fs::current_path() / "assets" / "swagger-ui";
  if (fs::exists(p1) && fs::is_directory(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "assets" / "swagger-ui";
  if (fs::exists(p2) && fs::is_directory(p2)) return p2;
  return std::nullopt;
}

bool is_safe_relpath(const std::filesystem::path& path) {
  if (path.is_absolute()) return false;
  for (const auto& part : path) {
    if (part == "..") return false;
  }
  return true;
}

std::string content_type_for_extension(const std::string& ext) {
  std::string lower;
  lower.reserve(ext.size());
  for (const char ch : ext) {
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  if (lower == ".html") return "text/html; charset=utf-8";
  if (lower == ".css") return "text/css; charset=utf-8";
  if (lower == ".js") return "application/javascript";
  if (lower == ".json") return "application/json";
  if (lower == ".yaml" || lower == ".yml") return "application/yaml";
  if (lower == ".svg") return "image/svg+xml";
  if (lower == ".png") return "image/png";
  if (lower == ".ico") return "image/x-icon";
  if (lower == ".txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return std::nullopt;
  std::ostringstream out;
  out << file.rdbuf();
  return out.str();
}

} // namespace

Session::Session(tcp::socket socket,
                 holder::store::Db& db,
                 const std::string& auth_token,
                 const Router& router,
                 std::chrono::steady_clock::time_point started_at,
                 holder::store::CardStore* card_store,
                 holder::index::FtsIndexer* fts)
    : socket_(std::move(socket)),
      db_(db),
      auth_token_(auth_token),
      router_(router),
      started_at_(started_at),
      card_store_(card_store),
      fts_(fts) {}

void Session::run() {
  namespace beast = boost::beast;
  namespace http = boost::beast::http;

  beast::flat_buffer buffer;
  http::request<http::string_body> req;
  boost::system::error_code ec;
  const auto request_started = std::chrono::steady_clock::now();

  http::read(socket_, buffer, req, ec);
  if (ec == http::error::end_of_stream) {
    socket_.shutdown(tcp::socket::shutdown_send, ec);
    return;
  }
  if (ec) {
    spdlog::warn("read failed: {}", ec.message());
    socket_.shutdown(tcp::socket::shutdown_send, ec);
    return;
  }

  const auto target = req.target();
  const std::string target_str(target.data(), target.size());

  const auto query_pos = target_str.find('?');
  const std::string path = (query_pos == std::string::npos)
                               ? target_str
                               : target_str.substr(0, query_pos);
  const std::string query_string =
      (query_pos == std::string::npos) ? "" : target_str.substr(query_pos + 1);

  http::response<http::string_body> res;

  if (path == "/openapi.yaml" || path == "/docs" || path.rfind("/docs/", 0) == 0) {
    if (req.method() != http::verb::get) {
      res = text_response(http::status::method_not_allowed,
                          "Method not allowed.",
                          "text/plain; charset=utf-8");
    } else if (path == "/openapi.yaml") {
      const auto openapi_path = find_openapi_path();
      if (!openapi_path.has_value()) {
        res = text_response(http::status::not_found,
                            "openapi.yaml not found.",
                            "text/plain; charset=utf-8");
      } else {
        const auto content = read_file(openapi_path.value());
        if (!content.has_value()) {
          res = text_response(http::status::not_found,
                              "openapi.yaml not found.",
                              "text/plain; charset=utf-8");
        } else {
          res = text_response(http::status::ok,
                              content.value(),
                              content_type_for_extension(openapi_path->extension().string()));
        }
      }
    } else {
      const auto docs_root = find_docs_root();
      if (!docs_root.has_value()) {
        res = text_response(http::status::not_found,
                            "Docs assets not found.",
                            "text/plain; charset=utf-8");
      } else {
        std::string rel = "index.html";
        if (path.rfind("/docs/", 0) == 0) {
          rel = path.substr(std::string("/docs/").size());
          if (rel.empty()) rel = "index.html";
        }
        std::filesystem::path rel_path(rel);
        if (!is_safe_relpath(rel_path)) {
          res = text_response(http::status::not_found,
                              "Not found.",
                              "text/plain; charset=utf-8");
        } else {
          const auto full_path = docs_root.value() / rel_path;
          const auto content = read_file(full_path);
          if (!content.has_value()) {
            res = text_response(http::status::not_found,
                                "Not found.",
                                "text/plain; charset=utf-8");
          } else {
            res = text_response(http::status::ok,
                                content.value(),
                                content_type_for_extension(full_path.extension().string()));
          }
        }
      }
    }
  } else if (!is_authorized(req, auth_token_)) {
    res = error_response(http::status::unauthorized, "unauthorized", "Missing or invalid token.");
  } else if (router_.dispatch(req, res)) {
    // handled
  } else {
    auto param = [&](const std::string& key) -> std::string {
      const std::string needle = key + "=";
      const auto pos = query_string.find(needle);
      if (pos == std::string::npos) return {};
      const auto start = pos + needle.size();
      const auto end = query_string.find('&', start);
      return query_string.substr(start, end == std::string::npos ? std::string::npos : end - start);
    };

    if (path == "/search/cards" && req.method() == http::verb::get) {
      if (!fts_) {
        res = error_response(http::status::not_implemented, "not_implemented", "Search unavailable.");
      } else {
        const std::string project_id = param("project_id");
        const std::string q = param("q");
        int limit = 20;
        int offset = 0;
        if (!param("limit").empty()) limit = std::stoi(param("limit"));
        if (!param("offset").empty()) offset = std::stoi(param("offset"));

        if (project_id.empty() || q.empty()) {
          res = error_response(http::status::bad_request, "bad_request", "Missing project_id or q.");
        } else {
          try {
            const auto rows = fts_->search_cards(project_id, q, limit, offset);
            nlohmann::json data = nlohmann::json::array();
            for (const auto& row : rows) {
              data.push_back({
                {"card_id", row.id},
                {"title", row.title},
                {"updated_at", row.updated_at},
                {"created_at", row.created_at},
                {"snippet", row.snippet},
                {"rank", row.rank}
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
    } else if (path == "/search/ai" && req.method() == http::verb::get) {
      if (!fts_) {
        res = error_response(http::status::not_implemented, "not_implemented", "Search unavailable.");
      } else {
        const std::string project_id = param("project_id");
        const std::string q = param("q");
        int limit = 20;
        int offset = 0;
        if (!param("limit").empty()) limit = std::stoi(param("limit"));
        if (!param("offset").empty()) offset = std::stoi(param("offset"));

        if (project_id.empty() || q.empty()) {
          res = error_response(http::status::bad_request, "bad_request", "Missing project_id or q.");
        } else {
          try {
            const auto rows = fts_->search_messages(project_id, q, limit, offset);
            nlohmann::json data = nlohmann::json::array();
            for (const auto& row : rows) {
              data.push_back({
                {"message_id", row.id},
                {"created_at", row.created_at},
                {"snippet", row.snippet},
                {"rank", row.rank}
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
    } else if (path == "/cards" && req.method() == http::verb::post) {
      if (!card_store_) {
        res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
      } else {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("card_id") || !body.contains("project_id") ||
              !body.contains("title") || !body.contains("content") ||
              !body.contains("created_at") || !body.contains("updated_at")) {
            res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
          } else {
            holder::model::Card card;
            card.card_id = body.at("card_id").get<std::string>();
            card.project_id = body.at("project_id").get<std::string>();
            card.title = body.at("title").get<std::string>();
            card.created_at = body.at("created_at").get<long long>();
            card.updated_at = body.at("updated_at").get<long long>();
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
            card_store_->create(card, content);

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
    } else if (path.rfind("/cards/", 0) == 0) {
      const std::string card_id = path.substr(std::string("/cards/").size());
      if (card_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (!card_store_) {
        res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
      } else if (req.method() == http::verb::get) {
        try {
          const auto card_opt = card_store_->get(card_id);
          if (!card_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Card not found.");
          } else {
            const auto& card = card_opt.value();
            const auto content_opt = card_store_->get_content(card);
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
              if (card.parent_card_id.has_value()) {
                data["parent_card_id"] = card.parent_card_id.value();
              } else {
                data["parent_card_id"] = nullptr;
              }
              if (card.deleted_at.has_value()) {
                data["deleted_at"] = card.deleted_at.value();
              } else {
                data["deleted_at"] = nullptr;
              }
              data["content"] = content_opt.value();

              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = data;

              res = json_response(http::status::ok, payload);
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

            card_store_->update_content(card_id, content, title, updated_at);

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"card_id", card_id}};

            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      }
    } else {
      res = error_response(http::status::not_found, "not_found", "Route not found.");
    }
  }

  http::write(socket_, res, ec);
  if (ec) {
    spdlog::warn("write failed: {}", ec.message());
  }

  const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - request_started)
                               .count();
  spdlog::info("HTTP {} {} -> {} ({}ms)",
               req.method_string(),
               req.target(),
               res.result_int(),
               duration_ms);

  socket_.shutdown(tcp::socket::shutdown_send, ec);
}

} // namespace holder::api
