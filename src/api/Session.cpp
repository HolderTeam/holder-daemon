#include "api/Session.h"

#include "core/CardPaths.h"
#include "core/ServerInfo.h"
#include "store/CardRepo.h"
#include "store/ProjectRepo.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

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

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string generate_uuid_v4() {
  boost::uuids::random_generator gen;
  return boost::uuids::to_string(gen());
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

    if (path == "/projects" && req.method() == http::verb::get) {
      try {
        holder::store::ProjectRepo repo(db_);
        auto projects = repo.list();
        const std::string name_filter = param("name");
        const std::string updated_after_raw = param("updated_after");
        const std::string updated_before_raw = param("updated_before");
        const std::string limit_raw = param("limit");
        const std::string offset_raw = param("offset");
        const std::string order_raw = param("order");
        std::optional<long long> updated_after;
        std::optional<long long> updated_before;
        if (!updated_after_raw.empty()) {
          updated_after = std::stoll(updated_after_raw);
        }
        if (!updated_before_raw.empty()) {
          updated_before = std::stoll(updated_before_raw);
        }
        int limit = 100;
        int offset = 0;
        if (!limit_raw.empty()) {
          limit = std::stoi(limit_raw);
        }
        if (!offset_raw.empty()) {
          offset = std::stoi(offset_raw);
        }
        if (limit < 0 || offset < 0) {
          throw std::invalid_argument("limit/offset must be non-negative");
        }
        if (limit > 1000) {
          limit = 1000;
        }
        enum class OrderKey { UpdatedAt, CreatedAt, Name };
        OrderKey order_key = OrderKey::UpdatedAt;
        bool order_asc = false;
        if (!order_raw.empty()) {
          if (order_raw == "updated_at_desc") {
            order_asc = false;
            order_key = OrderKey::UpdatedAt;
          } else if (order_raw == "updated_at_asc") {
            order_asc = true;
            order_key = OrderKey::UpdatedAt;
          } else if (order_raw == "created_at_desc") {
            order_asc = false;
            order_key = OrderKey::CreatedAt;
          } else if (order_raw == "created_at_asc") {
            order_asc = true;
            order_key = OrderKey::CreatedAt;
          } else if (order_raw == "name_desc") {
            order_asc = false;
            order_key = OrderKey::Name;
          } else if (order_raw == "name_asc") {
            order_asc = true;
            order_key = OrderKey::Name;
          } else {
            throw std::invalid_argument(
                "order must be updated_at_desc, updated_at_asc, created_at_desc, created_at_asc, "
                "name_desc, or name_asc");
          }
        }

        if (!name_filter.empty() || updated_after.has_value() || updated_before.has_value()) {
          std::vector<holder::model::Project> filtered;
          filtered.reserve(projects.size());
          for (const auto& project : projects) {
            if (!name_filter.empty() && project.name.find(name_filter) == std::string::npos) {
              continue;
            }
            if (updated_after.has_value() && project.updated_at < updated_after.value()) {
              continue;
            }
            if (updated_before.has_value() && project.updated_at > updated_before.value()) {
              continue;
            }
            filtered.push_back(project);
          }
          projects = std::move(filtered);
        }

        std::stable_sort(projects.begin(), projects.end(),
                         [order_asc, order_key](const holder::model::Project& a,
                                                const holder::model::Project& b) {
                           switch (order_key) {
                             case OrderKey::CreatedAt:
                               return order_asc ? (a.created_at < b.created_at)
                                                : (a.created_at > b.created_at);
                             case OrderKey::Name:
                               return order_asc ? (a.name < b.name)
                                                : (a.name > b.name);
                             case OrderKey::UpdatedAt:
                             default:
                               return order_asc ? (a.updated_at < b.updated_at)
                                                : (a.updated_at > b.updated_at);
                           }
                         });

        if (offset > 0 || limit < static_cast<int>(projects.size())) {
          std::vector<holder::model::Project> paged;
          const std::size_t start = static_cast<std::size_t>(offset);
          if (start < projects.size()) {
            const std::size_t end = std::min(projects.size(),
                                             start + static_cast<std::size_t>(limit));
            paged.assign(projects.begin() + static_cast<std::ptrdiff_t>(start),
                         projects.begin() + static_cast<std::ptrdiff_t>(end));
          }
          projects = std::move(paged);
        }

        nlohmann::json data = nlohmann::json::array();
        for (const auto& project : projects) {
          data.push_back({
            {"project_id", project.project_id},
            {"name", project.name},
            {"root_path", project.root_path},
            {"created_at", project.created_at},
            {"updated_at", project.updated_at}
          });
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = json_response(http::status::ok, payload);
      } catch (const std::invalid_argument&) {
        res = error_response(http::status::bad_request, "bad_request", "Invalid filter value.");
      } catch (const std::out_of_range&) {
        res = error_response(http::status::bad_request, "bad_request", "Filter value out of range.");
      } catch (const std::exception& ex) {
        res = error_response(http::status::internal_server_error, "error", ex.what());
      }
    } else if (path == "/projects" && req.method() == http::verb::post) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("name") || !body.contains("root_path")) {
          res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
        } else {
          holder::model::Project project;
          if (body.contains("project_id") && !body.at("project_id").is_null()) {
            project.project_id = body.at("project_id").get<std::string>();
          }
          if (project.project_id.empty()) {
            project.project_id = generate_uuid_v4();
          }
          project.name = body.at("name").get<std::string>();
          project.root_path = body.at("root_path").get<std::string>();
          if (body.contains("created_at") && !body.at("created_at").is_null()) {
            project.created_at = body.at("created_at").get<long long>();
          }
          if (body.contains("updated_at") && !body.at("updated_at").is_null()) {
            project.updated_at = body.at("updated_at").get<long long>();
          }
          if (project.created_at <= 0) {
            project.created_at = now_epoch_seconds();
          }
          if (project.updated_at <= 0) {
            project.updated_at = project.created_at;
          }

          holder::store::ProjectRepo repo(db_);
          repo.create(project);

          nlohmann::json data;
          data["project_id"] = project.project_id;
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;

          res = json_response(http::status::created, payload);
        }
      } catch (const std::exception& ex) {
        res = error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (path.rfind("/projects/", 0) == 0) {
      const std::string project_id = path.substr(std::string("/projects/").size());
      if (project_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (req.method() == http::verb::get) {
        try {
          holder::store::ProjectRepo repo(db_);
          const auto project_opt = repo.get(project_id);
          if (!project_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Project not found.");
          } else {
            const auto& project = project_opt.value();
            nlohmann::json data;
            data["project_id"] = project.project_id;
            data["name"] = project.name;
            data["root_path"] = project.root_path;
            data["created_at"] = project.created_at;
            data["updated_at"] = project.updated_at;

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = data;
            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::internal_server_error, "error", ex.what());
        }
      } else if (req.method() == http::verb::patch) {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("updated_at")) {
            res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
          } else {
            const long long updated_at = body.at("updated_at").get<long long>();
            const bool has_name = body.contains("name") && !body.at("name").is_null();
            const bool has_root = body.contains("root_path") && !body.at("root_path").is_null();
            if (!has_name && !has_root) {
              res = error_response(http::status::bad_request, "bad_request", "No fields to update.");
            } else {
              holder::store::ProjectRepo repo(db_);
              const auto project_opt = repo.get(project_id);
              if (!project_opt.has_value()) {
                res = error_response(http::status::not_found, "not_found", "Project not found.");
              } else {
                if (has_name) {
                  repo.update_name(project_id, body.at("name").get<std::string>(), updated_at);
                }
                if (has_root) {
                  repo.update_root_path(project_id, body.at("root_path").get<std::string>(), updated_at);
                }
                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = {{"project_id", project_id}};
                res = json_response(http::status::ok, payload);
              }
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::delete_) {
        try {
          holder::store::ProjectRepo repo(db_);
          const auto project_opt = repo.get(project_id);
          if (!project_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Project not found.");
          } else {
            repo.remove(project_id);
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"project_id", project_id}};
            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::internal_server_error, "error", ex.what());
        }
      } else {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      }
    } else if (path == "/search/cards" && req.method() == http::verb::get) {
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
    } else if (path == "/cards" && req.method() == http::verb::get) {
      const std::string project_id = param("project_id");
      const std::string parent_raw = param("parent_card_id");
      if (project_id.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      } else {
        try {
          holder::store::CardRepo repo(db_);
          std::optional<std::string> parent;
          if (!parent_raw.empty()) {
            parent = parent_raw;
          }
          const auto cards = repo.list(project_id, parent);
          nlohmann::json data = nlohmann::json::array();
          for (const auto& card : cards) {
            nlohmann::json item;
            item["card_id"] = card.card_id;
            item["project_id"] = card.project_id;
            item["title"] = card.title;
            item["rel_path"] = card.rel_path;
            item["sort_key"] = card.sort_key;
            item["created_at"] = card.created_at;
            item["updated_at"] = card.updated_at;
            if (card.parent_card_id.has_value()) {
              item["parent_card_id"] = card.parent_card_id.value();
            } else {
              item["parent_card_id"] = nullptr;
            }
            if (card.deleted_at.has_value()) {
              item["deleted_at"] = card.deleted_at.value();
            } else {
              item["deleted_at"] = nullptr;
            }
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
    } else if (path == "/cards" && req.method() == http::verb::post) {
      if (!card_store_) {
        res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
      } else {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("project_id") || !body.contains("title") ||
              !body.contains("content")) {
            res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
          } else {
            holder::model::Card card;
            if (body.contains("card_id") && !body.at("card_id").is_null()) {
              card.card_id = body.at("card_id").get<std::string>();
            }
            if (card.card_id.empty()) {
              card.card_id = generate_uuid_v4();
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
