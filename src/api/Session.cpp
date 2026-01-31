#include "api/Session.h"

#include "core/CardPaths.h"
#include "core/ProjectPaths.h"
#include "git/GitOps.h"
#include "llm/LocalModelRunner.h"
#include "core/ServerInfo.h"
#include "store/AiThreadRepo.h"
#include "store/CardRepo.h"
#include "store/AiMessageRepo.h"
#include "store/AiRunRepo.h"
#include "store/LinkRepo.h"
#include "store/ProjectRepo.h"
#include "store/Rebuilder.h"
#include "store/ResourceRepo.h"

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
#include <algorithm>
#include <mutex>
#include <optional>
#include <random>
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

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
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

std::optional<std::filesystem::path> find_models_path() {
  namespace fs = std::filesystem;
  if (const char* env = std::getenv("HOLDER_MODELS_PATH")) {
    fs::path p(env);
    if (fs::exists(p)) return p;
  }
  fs::path p1 = fs::current_path() / "models.yaml";
  if (fs::exists(p1)) return p1;
  fs::path p2 = fs::current_path().parent_path() / "models.yaml";
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

std::string truncate_bytes(const std::string& text, size_t max_bytes) {
  if (text.size() <= max_bytes) return text;
  return text.substr(0, max_bytes);
}

std::optional<std::string> extract_json_array(const std::string& text) {
  const auto start = text.find('[');
  if (start == std::string::npos) return std::nullopt;
  const auto end = text.find_last_of(']');
  if (end == std::string::npos || end <= start) return std::nullopt;
  return text.substr(start, end - start + 1);
}

std::vector<std::string> parse_ranked_models(const std::string& text,
                                             const std::vector<std::string>& candidates) {
  const auto array_text = extract_json_array(text);
  if (!array_text.has_value()) return {};
  try {
    const auto parsed = nlohmann::json::parse(array_text.value());
    if (!parsed.is_array()) return {};
    std::vector<std::string> ranked;
    for (const auto& item : parsed) {
      if (!item.is_string()) continue;
      const auto name = item.get<std::string>();
      if (std::find(candidates.begin(), candidates.end(), name) != candidates.end()) {
        if (std::find(ranked.begin(), ranked.end(), name) == ranked.end()) {
          ranked.push_back(name);
        }
      }
    }
    return ranked;
  } catch (const std::exception&) {
    return {};
  }
}

std::string pick_smallest_model(const std::vector<holder::llm::LocalModel>& models) {
  if (models.empty()) return {};
  const auto* best = &models.front();
  for (const auto& model : models) {
    if (model.size > 0 && (best->size == 0 || model.size < best->size)) {
      best = &model;
    }
  }
  return best->name;
}

std::string pick_largest_model(const std::vector<holder::llm::LocalModel>& models) {
  if (models.empty()) return {};
  const auto* best = &models.front();
  for (const auto& model : models) {
    if (model.size > best->size) {
      best = &model;
    }
  }
  return best->name;
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
  if (const char* seed_env = std::getenv("HOLDER_UUID_SEED")) {
    try {
      const auto seed = static_cast<unsigned int>(std::stoul(seed_env));
      static std::mutex mutex;
      static std::mt19937 rng;
      static bool seeded = false;
      std::lock_guard<std::mutex> lock(mutex);
      if (!seeded) {
        rng.seed(seed);
        seeded = true;
      }
      boost::uuids::basic_random_generator<std::mt19937> gen(&rng);
      return boost::uuids::to_string(gen());
    } catch (const std::exception&) {
      // Fall through to random generator.
    }
  }
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
                 holder::index::FtsIndexer* fts,
                 holder::git::GitOps* git_ops,
                 holder::llm::LocalModelRunner* runner)
    : socket_(std::move(socket)),
      db_(db),
      auth_token_(auth_token),
      router_(router),
      started_at_(started_at),
      card_store_(card_store),
      fts_(fts),
      git_ops_(git_ops),
      runner_(runner) {}

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

  if (path == "/openapi.yaml" || path == "/models.yaml" || path == "/docs" ||
      path.rfind("/docs/", 0) == 0) {
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
    } else if (path == "/models.yaml") {
      const auto models_path = find_models_path();
      if (!models_path.has_value()) {
        res = text_response(http::status::not_found,
                            "models.yaml not found.",
                            "text/plain; charset=utf-8");
      } else {
        const auto content = read_file(models_path.value());
        if (!content.has_value()) {
          res = text_response(http::status::not_found,
                              "models.yaml not found.",
                              "text/plain; charset=utf-8");
        } else {
          res = text_response(http::status::ok,
                              content.value(),
                              content_type_for_extension(models_path->extension().string()));
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
            {"git_remote_url", project.git_remote_url.has_value() ? nlohmann::json(project.git_remote_url.value())
                                                                  : nlohmann::json(nullptr)},
            {"git_provider", project.git_provider.has_value() ? nlohmann::json(project.git_provider.value())
                                                              : nlohmann::json(nullptr)},
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
        if (!body.contains("name")) {
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
          if (body.contains("root_path") && !body.at("root_path").is_null()) {
            project.root_path = body.at("root_path").get<std::string>();
          }
          if (body.contains("created_at") && !body.at("created_at").is_null()) {
            project.created_at = body.at("created_at").get<long long>();
          }
          if (body.contains("updated_at") && !body.at("updated_at").is_null()) {
            project.updated_at = body.at("updated_at").get<long long>();
          }
          if (body.contains("git_remote_url")) {
            if (!body.at("git_remote_url").is_null()) {
              project.git_remote_url = body.at("git_remote_url").get<std::string>();
            }
          }
          if (body.contains("git_provider")) {
            if (!body.at("git_provider").is_null()) {
              project.git_provider = body.at("git_provider").get<std::string>();
            }
          }
          if (project.created_at <= 0) {
            project.created_at = now_epoch_seconds();
          }
          if (project.updated_at <= 0) {
            project.updated_at = project.created_at;
          }

          holder::store::ProjectRepo repo(db_);
          if (project.root_path.empty()) {
            const auto base_root = holder::core::default_projects_root();
            const auto slug = holder::core::slugify(project.name);
            project.root_path = holder::core::unique_project_root(base_root, slug, repo.list());
          }
          repo.create(project);

          if (project.git_remote_url.has_value()) {
            holder::git::GitRepo git_repo;
            git_repo.open_or_init(project.root_path);
            git_repo.set_remote("origin", project.git_remote_url.value());
          }

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
            if (project.git_remote_url.has_value()) {
              data["git_remote_url"] = project.git_remote_url.value();
            } else {
              data["git_remote_url"] = nullptr;
            }
            if (project.git_provider.has_value()) {
              data["git_provider"] = project.git_provider.value();
            } else {
              data["git_provider"] = nullptr;
            }
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
            const bool has_git_remote = body.contains("git_remote_url");
            const bool has_git_provider = body.contains("git_provider");
            if (!has_name && !has_root && !has_git_remote && !has_git_provider) {
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
                if (has_git_remote) {
                  if (body.at("git_remote_url").is_null()) {
                    repo.update_git_remote(project_id, std::nullopt, updated_at);
                  } else {
                    repo.update_git_remote(
                        project_id,
                        std::optional<std::string>(body.at("git_remote_url").get<std::string>()),
                        updated_at);
                  }
                }
                if (has_git_provider) {
                  if (body.at("git_provider").is_null()) {
                    repo.update_git_provider(project_id, std::nullopt, updated_at);
                  } else {
                    repo.update_git_provider(
                        project_id,
                        std::optional<std::string>(body.at("git_provider").get<std::string>()),
                        updated_at);
                  }
                }
                if (has_git_remote) {
                  const std::string repo_root =
                      has_root ? body.at("root_path").get<std::string>()
                               : project_opt->root_path;
                  auto& git = resolve_git(git_ops_);
                  git.open_or_init(repo_root);
                  if (body.at("git_remote_url").is_null()) {
                    git.remove_remote("origin");
                  } else {
                    git.set_remote("origin",
                                   body.at("git_remote_url").get<std::string>());
                  }
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
    } else if (path == "/rebuild" && req.method() == http::verb::post) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("project_id")) {
          res = error_response(http::status::bad_request, "bad_request", "Missing project_id.");
        } else {
          const std::string project_id = body.at("project_id").get<std::string>();
          holder::store::ProjectRepo repo(db_);
          const auto project_opt = repo.get(project_id);
          if (!project_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Project not found.");
          } else {
            holder::store::Rebuilder rebuilder(db_, fts_);
            const auto stats = rebuilder.rebuild_project(project_opt.value());
            nlohmann::json data;
            data["project_id"] = project_id;
            data["cards"] = stats.cards;
            data["ai_messages"] = stats.ai_messages;
            data["ai_threads"] = stats.ai_threads;
            data["links"] = stats.links;
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = data;
            res = json_response(http::status::ok, payload);
          }
        }
      } catch (const std::exception& ex) {
        res = error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (path == "/search/cards" && req.method() == http::verb::get) {
      if (!fts_) {
        res = error_response(http::status::not_implemented, "not_implemented", "Search unavailable.");
      } else {
        const std::string project_id = param("project_id");
        const std::string q = param("q");
        int limit = 20;
        int offset = 0;
        bool bad_params = false;
        try {
          if (!param("limit").empty()) limit = std::stoi(param("limit"));
          if (!param("offset").empty()) offset = std::stoi(param("offset"));
        } catch (const std::exception&) {
          res = error_response(http::status::bad_request,
                               "bad_request",
                               "Invalid limit/offset.");
          bad_params = true;
        }

        if (!bad_params) {
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
      }
    } else if (path == "/search/ai" && req.method() == http::verb::get) {
      if (!fts_) {
        res = error_response(http::status::not_implemented, "not_implemented", "Search unavailable.");
      } else {
        const std::string project_id = param("project_id");
        const std::string q = param("q");
        int limit = 20;
        int offset = 0;
        bool bad_params = false;
        try {
          if (!param("limit").empty()) limit = std::stoi(param("limit"));
          if (!param("offset").empty()) offset = std::stoi(param("offset"));
        } catch (const std::exception&) {
          res = error_response(http::status::bad_request,
                               "bad_request",
                               "Invalid limit/offset.");
          bad_params = true;
        }

        if (!bad_params) {
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
      }
    } else if (path == "/ai/capabilities" && req.method() == http::verb::get) {
      nlohmann::json data;
      if (!runner_) {
        data["runner_available"] = false;
        data["error"] = "Local model runner not configured.";
        data["last_checked"] = now_epoch_seconds();
      } else {
        const auto status = runner_->status();
        data["runner_available"] = status.available;
        data["spawn_attempted"] = status.spawn_attempted;
        data["last_checked"] = status.last_checked;
        data["version"] = status.version;
        data["error"] = status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
        nlohmann::json models = nlohmann::json::array();
        for (const auto& model : status.models) {
          models.push_back({
            {"name", model.name},
            {"digest", model.digest},
            {"size", model.size},
            {"modified_at", model.modified_at},
          });
        }
        data["models"] = models;
      }
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = data;
      res = json_response(http::status::ok, payload);
    } else if (path == "/ai/runner/retry" && req.method() == http::verb::post) {
      if (!runner_) {
        res = error_response(http::status::not_implemented,
                             "not_implemented",
                             "Local model runner not configured.");
      } else {
        const auto status = runner_->retry();
        nlohmann::json data;
        data["runner_available"] = status.available;
        data["spawn_attempted"] = status.spawn_attempted;
        data["last_checked"] = status.last_checked;
        data["version"] = status.version;
        data["error"] = status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
        nlohmann::json models = nlohmann::json::array();
        for (const auto& model : status.models) {
          models.push_back({
            {"name", model.name},
            {"digest", model.digest},
            {"size", model.size},
            {"modified_at", model.modified_at},
          });
        }
        data["models"] = models;
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = json_response(http::status::ok, payload);
      }
    } else if (path == "/ai/complete" && req.method() == http::verb::post) {
      if (!runner_) {
        res = error_response(http::status::not_implemented,
                             "not_implemented",
                             "Local model runner not configured.");
      } else {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("prompt")) {
            res = error_response(http::status::bad_request, "bad_request", "Missing prompt.");
          } else {
            const std::string prompt = body.at("prompt").get<std::string>();
            std::string mode = "auto";
            if (body.contains("mode")) {
              mode = body.at("mode").get<std::string>();
            }
            std::optional<std::string> project_id;
            if (body.contains("project_id") && !body.at("project_id").is_null()) {
              project_id = body.at("project_id").get<std::string>();
            }
            std::optional<std::string> thread_id;
            if (body.contains("thread_id") && !body.at("thread_id").is_null()) {
              thread_id = body.at("thread_id").get<std::string>();
            }
            std::string context_json;
            std::optional<std::string> context_card_id;
            if (body.contains("context") && !body.at("context").is_null()) {
              context_json = body.at("context").dump();
              if (body.at("context").is_object() && body.at("context").contains("card_id") &&
                  !body.at("context").at("card_id").is_null()) {
                context_card_id = body.at("context").at("card_id").get<std::string>();
              }
            }

            const auto runner_status = runner_->status();
            if (!runner_status.available) {
              res = error_response(http::status::service_unavailable,
                                   "runner_unavailable",
                                   "Local model runner unavailable.");
            } else if (runner_status.models.empty()) {
              res = error_response(http::status::service_unavailable,
                                   "runner_unavailable",
                                   "No local models installed.");
            } else {
              std::string forced_model;
              if (body.contains("model") && !body.at("model").is_null()) {
                forced_model = body.at("model").get<std::string>();
                mode = "model";
              }

              std::vector<std::string> candidates;
              candidates.reserve(runner_status.models.size());
              for (const auto& model : runner_status.models) {
                candidates.push_back(model.name);
              }

              if (!thread_id.has_value() && project_id.has_value()) {
                holder::store::AiThreadRepo thread_repo(db_);
                holder::model::AiThread thread;
                thread.thread_id = generate_uuid_v4();
                thread.project_id = project_id.value();
                if (context_card_id.has_value()) {
                  thread.card_id = context_card_id.value();
                }
                std::string title = prompt;
                if (title.size() > 80) {
                  title = title.substr(0, 80);
                }
                thread.title = title;
                thread.created_at = now_epoch_seconds();
                thread.updated_at = thread.created_at;
                thread_repo.create(thread);
                thread_id = thread.thread_id;
              }

              std::string router_model;
              if (forced_model.empty() && candidates.size() > 1) {
                router_model = pick_smallest_model(runner_status.models);
              }

              holder::store::AiRunRepo run_repo(db_);
              holder::model::AiRun run;
              run.run_id = generate_uuid_v4();
              run.project_id = project_id;
              run.thread_id = thread_id;
              run.mode = (mode == "model") ? "model" : "auto";
              run.prompt = prompt;
              if (!context_json.empty()) {
                run.context_json = context_json;
              }
              if (!router_model.empty()) {
                run.router_model = router_model;
              }
              run.status = "started";
              run.created_at = now_epoch_seconds();
              run.updated_at = run.created_at;
              run_repo.create(run);

              http::response<http::empty_body> sse{http::status::ok, 11};
              sse.set(http::field::content_type, "text/event-stream");
              sse.set(http::field::cache_control, "no-cache");
              sse.set(http::field::connection, "keep-alive");
              sse.keep_alive(true);
              http::serializer<false, http::empty_body> sr{sse};
              boost::system::error_code write_ec;
              http::write_header(socket_, sr, write_ec);
              if (write_ec) {
                return;
              }

              auto send_event = [&](const std::string& name, const nlohmann::json& data) -> bool {
                std::string payload = "event: " + name + "\n";
                payload += "data: " + data.dump() + "\n\n";
                boost::system::error_code send_ec;
                boost::asio::write(socket_, boost::asio::buffer(payload), send_ec);
                return !send_ec;
              };

              std::string ranked_json;
              std::string chosen_model;
              std::string assistant_text;

              if (thread_id.has_value()) {
                holder::store::AiMessageRepo msg_repo(db_, fts_);
                holder::model::AiMessage user_msg;
                user_msg.message_id = generate_uuid_v4();
                user_msg.thread_id = thread_id.value();
                user_msg.role = "user";
                user_msg.source = "local";
                user_msg.content = prompt;
                user_msg.created_at = now_epoch_seconds();
                msg_repo.append(user_msg);
              }

              if (!forced_model.empty()) {
                send_event("router", {{"message", "routing skipped (forced model)"}});
                candidates = {forced_model};
              } else if (candidates.size() == 1) {
                send_event("router", {{"message", "routing skipped (single model)"}});
              } else {
                if (router_model.empty()) {
                  router_model = pick_smallest_model(runner_status.models);
                }
                const size_t router_limit = 50000;
                std::string router_input = prompt;
                if (!context_json.empty()) {
                  router_input += "\n\nContext:\n";
                  router_input += truncate_bytes(context_json, router_limit);
                }

                std::ostringstream prompt_ss;
                prompt_ss << "You are a routing assistant. Output ONLY a JSON array of model names "
                          << "ranked by best fit for the user prompt.\n";
                prompt_ss << "User prompt:\n" << router_input << "\n\n";
                prompt_ss << "Candidate models:\n";
                for (const auto& model : runner_status.models) {
                  prompt_ss << "- " << model.name << " (size=" << model.size << ")\n";
                }

                std::string router_text;
                std::string router_error;
                nlohmann::json router_options;
                router_options["temperature"] = 0.1;
                router_options["num_predict"] = 256;

                runner_->stream_generate(
                    router_model,
                    prompt_ss.str(),
                    router_options.dump(),
                    [&](const std::string& chunk) {
                      router_text += chunk;
                      send_event("router", {{"delta", chunk}});
                    },
                    &router_error);

                auto ranked = parse_ranked_models(router_text, candidates);
                if (ranked.empty()) {
                  ranked = candidates;
                  const std::string fallback = pick_largest_model(runner_status.models);
                  if (!fallback.empty()) {
                    ranked = {fallback};
                  }
                }
                nlohmann::json ranked_payload = ranked;
                ranked_json = ranked_payload.dump();
                send_event("router_result",
                           {{"router_model", router_model},
                            {"ranked", ranked_payload},
                            {"error", router_error.empty() ? nlohmann::json(nullptr)
                                                           : nlohmann::json(router_error)}});
                candidates = ranked;
              }

              std::string model_error;
              bool any_output = false;
              for (const auto& model : candidates) {
                send_event("progress", {{"message", "Trying model"}, {"model", model}});
                std::string prompt_full = prompt;
                if (!context_json.empty()) {
                  prompt_full += "\n\nContext:\n";
                  prompt_full += context_json;
                }
                bool got_output = false;
                model_error.clear();
                const bool ok = runner_->stream_generate(
                    model,
                    prompt_full,
                    std::string(),
                    [&](const std::string& chunk) {
                      got_output = true;
                      any_output = true;
                      assistant_text += chunk;
                      send_event("chunk", {{"model", model}, {"delta", chunk}});
                    },
                    &model_error);

                if (ok && got_output) {
                  chosen_model = model;
                  break;
                }

                nlohmann::json fallback_event = {{"model", model}};
                if (!model_error.empty()) fallback_event["error"] = model_error;
                if (got_output && !ok) fallback_event["partial"] = true;
                send_event("fallback", fallback_event);
              }

              const long long updated_at = now_epoch_seconds();
              if (!chosen_model.empty()) {
                send_event("done", {{"model", chosen_model}});
                std::optional<std::string> message_id;
                if (thread_id.has_value()) {
                  holder::store::AiMessageRepo msg_repo(db_, fts_);
                  holder::model::AiMessage assistant_msg;
                  assistant_msg.message_id = generate_uuid_v4();
                  assistant_msg.thread_id = thread_id.value();
                  assistant_msg.role = "assistant";
                  assistant_msg.source = "local";
                  assistant_msg.provider = "LocalRunner";
                  assistant_msg.model = chosen_model;
                  assistant_msg.content = assistant_text;
                  assistant_msg.created_at = updated_at;
                  msg_repo.append(assistant_msg);
                  message_id = assistant_msg.message_id;
                }
                run_repo.update_status(run.run_id,
                                       "completed",
                                       std::nullopt,
                                       message_id,
                                       chosen_model,
                                       ranked_json.empty() ? std::nullopt
                                                           : std::optional<std::string>(ranked_json),
                                       updated_at);
              } else {
                send_event("failed", {{"error", "All models failed."}});
                run_repo.update_status(run.run_id,
                                       "failed",
                                       std::optional<std::string>(any_output ? "partial failure"
                                                                             : "no output"),
                                       std::nullopt,
                                       std::nullopt,
                                       ranked_json.empty() ? std::nullopt
                                                           : std::optional<std::string>(ranked_json),
                                       updated_at);
              }
              return;
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    } else if (path == "/ai/runs" && req.method() == http::verb::get) {
      const std::string project_id = param("project_id");
      const std::string thread_id = param("thread_id");
      if (project_id.empty() && thread_id.empty()) {
        res = error_response(http::status::bad_request,
                             "bad_request",
                             "Missing project_id or thread_id.");
      } else {
        try {
          holder::store::AiRunRepo repo(db_);
          std::vector<holder::model::AiRun> runs;
          if (!thread_id.empty()) {
            runs = repo.list_by_thread(thread_id);
          } else {
            runs = repo.list_by_project(project_id);
          }
          nlohmann::json data = nlohmann::json::array();
          for (const auto& run : runs) {
            nlohmann::json item;
            item["run_id"] = run.run_id;
            item["project_id"] = run.project_id.has_value() ? nlohmann::json(run.project_id.value())
                                                            : nlohmann::json(nullptr);
            item["thread_id"] = run.thread_id.has_value() ? nlohmann::json(run.thread_id.value())
                                                          : nlohmann::json(nullptr);
            item["message_id"] = run.message_id.has_value() ? nlohmann::json(run.message_id.value())
                                                            : nlohmann::json(nullptr);
            item["mode"] = run.mode;
            item["prompt"] = run.prompt;
            item["context_json"] =
                run.context_json.has_value() ? nlohmann::json(run.context_json.value())
                                             : nlohmann::json(nullptr);
            item["router_model"] = run.router_model.has_value() ? nlohmann::json(run.router_model.value())
                                                                : nlohmann::json(nullptr);
            item["ranked_json"] = run.ranked_json.has_value() ? nlohmann::json(run.ranked_json.value())
                                                              : nlohmann::json(nullptr);
            item["chosen_model"] =
                run.chosen_model.has_value() ? nlohmann::json(run.chosen_model.value())
                                             : nlohmann::json(nullptr);
            item["status"] = run.status;
            item["error"] = run.error.has_value() ? nlohmann::json(run.error.value())
                                                  : nlohmann::json(nullptr);
            item["created_at"] = run.created_at;
            item["updated_at"] = run.updated_at;
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
    } else if (path.rfind("/ai/runs/", 0) == 0 && req.method() == http::verb::get) {
      const std::string run_id = path.substr(std::string("/ai/runs/").size());
      if (run_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Run not found.");
      } else {
        try {
          holder::store::AiRunRepo repo(db_);
          const auto run = repo.get(run_id);
          if (!run.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Run not found.");
          } else {
            const auto& r = run.value();
            nlohmann::json data;
            data["run_id"] = r.run_id;
            data["project_id"] = r.project_id.has_value() ? nlohmann::json(r.project_id.value())
                                                          : nlohmann::json(nullptr);
            data["thread_id"] = r.thread_id.has_value() ? nlohmann::json(r.thread_id.value())
                                                        : nlohmann::json(nullptr);
            data["message_id"] = r.message_id.has_value() ? nlohmann::json(r.message_id.value())
                                                          : nlohmann::json(nullptr);
            data["mode"] = r.mode;
            data["prompt"] = r.prompt;
            data["context_json"] = r.context_json.has_value() ? nlohmann::json(r.context_json.value())
                                                              : nlohmann::json(nullptr);
            data["router_model"] = r.router_model.has_value() ? nlohmann::json(r.router_model.value())
                                                              : nlohmann::json(nullptr);
            data["ranked_json"] = r.ranked_json.has_value() ? nlohmann::json(r.ranked_json.value())
                                                            : nlohmann::json(nullptr);
            data["chosen_model"] = r.chosen_model.has_value() ? nlohmann::json(r.chosen_model.value())
                                                              : nlohmann::json(nullptr);
            data["status"] = r.status;
            data["error"] = r.error.has_value() ? nlohmann::json(r.error.value())
                                                : nlohmann::json(nullptr);
            data["created_at"] = r.created_at;
            data["updated_at"] = r.updated_at;
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = data;
            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    } else if (path.rfind("/ai/runner/pull/", 0) == 0 &&
               path.size() > std::string("/ai/runner/pull/").size() + std::string("/events").size() &&
               path.compare(path.size() - std::string("/events").size(),
                            std::string("/events").size(),
                            "/events") == 0 &&
               req.method() == http::verb::get) {
      if (!runner_) {
        res = error_response(http::status::not_implemented,
                             "not_implemented",
                             "Local model runner not configured.");
      } else {
        const std::string prefix = "/ai/runner/pull/";
        const std::string suffix = "/events";
        const std::string job_id =
            path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
        if (job_id.empty()) {
          res = error_response(http::status::not_found, "not_found", "Pull job not found.");
        } else {
          http::response<http::empty_body> sse{http::status::ok, 11};
          sse.set(http::field::content_type, "text/event-stream");
          sse.set(http::field::cache_control, "no-cache");
          sse.set(http::field::connection, "keep-alive");
          sse.keep_alive(true);
          http::serializer<false, http::empty_body> sr{sse};
          boost::system::error_code write_ec;
          http::write_header(socket_, sr, write_ec);
          if (write_ec) {
            return;
          }

          auto send_event = [&](const std::string& name, const nlohmann::json& data) -> bool {
            std::string payload = "event: " + name + "\n";
            payload += "data: " + data.dump() + "\n\n";
            boost::system::error_code send_ec;
            boost::asio::write(socket_, boost::asio::buffer(payload), send_ec);
            return !send_ec;
          };

          std::string last_status;
          long long last_completed = -1;
          long long last_total = -1;
          for (;;) {
            const auto job = runner_->get_pull(job_id);
            if (!job.has_value()) {
              send_event("failed", nlohmann::json{{"error", "Pull job not found."}});
              break;
            }

            bool changed = job->status != last_status ||
                           job->progress.completed != last_completed ||
                           job->progress.total != last_total;
            if (changed) {
              nlohmann::json data;
              data["job_id"] = job->job_id;
              data["model"] = job->model;
              data["status"] = job->status;
              data["updated_at"] = job->updated_at;
              data["error"] = job->error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job->error);
              data["progress"] = {
                {"completed", job->progress.completed},
                {"total", job->progress.total},
                {"percent", job->progress.percent},
                {"stage", job->progress.stage},
              };

              if (!send_event("progress", data)) {
                break;
              }

              last_status = job->status;
              last_completed = job->progress.completed;
              last_total = job->progress.total;

              if (job->status == "completed") {
                send_event("completed", data);
                break;
              }
              if (job->status == "failed") {
                send_event("failed", data);
                break;
              }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }
          return;
        }
      }
    } else if (path == "/ai/runner/pull" && req.method() == http::verb::post) {
      if (!runner_) {
        res = error_response(http::status::not_implemented,
                             "not_implemented",
                             "Local model runner not configured.");
      } else {
        try {
          const auto runner_status = runner_->status();
          if (!runner_status.available) {
            res = error_response(http::status::service_unavailable,
                                 "runner_unavailable",
                                 "Local model runner unavailable.");
          } else {
            const auto body = nlohmann::json::parse(req.body());
            if (!body.contains("model")) {
              res = error_response(http::status::bad_request, "bad_request", "Missing model.");
            } else {
              const std::string model = body.at("model").get<std::string>();
              auto job = runner_->start_pull(model);
              if (job.status == "failed") {
                res = error_response(http::status::bad_request,
                                     "bad_request",
                                     job.error.empty() ? "Pull failed." : job.error);
              } else {
                nlohmann::json data;
                data["job_id"] = job.job_id;
                data["model"] = job.model;
                data["status"] = job.status;
                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = data;
                res = json_response(http::status::ok, payload);
              }
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    } else if (path.rfind("/ai/runner/pull/", 0) == 0 && req.method() == http::verb::get) {
      if (!runner_) {
        res = error_response(http::status::not_implemented,
                             "not_implemented",
                             "Local model runner not configured.");
      } else {
        const std::string job_id = path.substr(std::string("/ai/runner/pull/").size());
        if (job_id.empty()) {
          res = error_response(http::status::not_found, "not_found", "Pull job not found.");
        } else {
          const auto job = runner_->get_pull(job_id);
          if (!job.has_value()) {
            res = error_response(http::status::not_found, "not_found", "Pull job not found.");
          } else {
            nlohmann::json data;
            data["job_id"] = job->job_id;
            data["model"] = job->model;
            data["status"] = job->status;
            data["updated_at"] = job->updated_at;
            data["error"] = job->error.empty() ? nlohmann::json(nullptr) : nlohmann::json(job->error);
            nlohmann::json progress;
            progress["completed"] = job->progress.completed;
            progress["total"] = job->progress.total;
            progress["percent"] = job->progress.percent;
            progress["stage"] = job->progress.stage;
            data["progress"] = progress;
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = data;
            res = json_response(http::status::ok, payload);
          }
        }
      }
    } else if (path == "/ai/threads" && req.method() == http::verb::get) {
      const std::string project_id = param("project_id");
      if (project_id.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      } else {
        try {
          holder::store::AiThreadRepo repo(db_);
          const auto threads = repo.list(project_id);
          nlohmann::json data = nlohmann::json::array();
          for (const auto& thread : threads) {
            nlohmann::json item;
            item["thread_id"] = thread.thread_id;
            item["project_id"] = thread.project_id;
            item["title"] = thread.title;
            item["created_at"] = thread.created_at;
            item["updated_at"] = thread.updated_at;
            if (thread.card_id.has_value()) {
              item["card_id"] = thread.card_id.value();
            } else {
              item["card_id"] = nullptr;
            }
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
    } else if (path == "/ai/threads" && req.method() == http::verb::post) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("project_id") || !body.contains("title")) {
          res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
        } else {
          holder::model::AiThread thread;
          if (body.contains("thread_id") && !body.at("thread_id").is_null()) {
            thread.thread_id = body.at("thread_id").get<std::string>();
          }
          if (thread.thread_id.empty()) {
            thread.thread_id = generate_uuid_v4();
          }
          thread.project_id = body.at("project_id").get<std::string>();
          thread.title = body.at("title").get<std::string>();
          if (body.contains("card_id") && !body.at("card_id").is_null()) {
            thread.card_id = body.at("card_id").get<std::string>();
          }
          if (body.contains("created_at") && !body.at("created_at").is_null()) {
            thread.created_at = body.at("created_at").get<long long>();
          }
          if (body.contains("updated_at") && !body.at("updated_at").is_null()) {
            thread.updated_at = body.at("updated_at").get<long long>();
          }
          if (thread.created_at <= 0) {
            thread.created_at = now_epoch_seconds();
          }
          if (thread.updated_at <= 0) {
            thread.updated_at = thread.created_at;
          }

          holder::store::AiThreadRepo repo(db_);
          repo.create(thread);

          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"thread_id", thread.thread_id}};
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
    } else if (path.rfind("/ai/threads/", 0) == 0) {
      const std::string thread_id = path.substr(std::string("/ai/threads/").size());
      if (thread_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (req.method() == http::verb::get) {
        try {
          holder::store::AiThreadRepo repo(db_);
          const auto thread_opt = repo.get(thread_id);
          if (!thread_opt.has_value()) {
            res = error_response(http::status::not_found, "not_found", "AI thread not found.");
          } else {
            const auto& thread = thread_opt.value();
            nlohmann::json data;
            data["thread_id"] = thread.thread_id;
            data["project_id"] = thread.project_id;
            data["title"] = thread.title;
            data["created_at"] = thread.created_at;
            data["updated_at"] = thread.updated_at;
            if (thread.card_id.has_value()) {
              data["card_id"] = thread.card_id.value();
            } else {
              data["card_id"] = nullptr;
            }
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = data;
            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::patch) {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("updated_at")) {
            res = error_response(http::status::bad_request, "bad_request", "Missing updated_at.");
          } else {
            std::optional<std::string> title;
            if (body.contains("title") && !body.at("title").is_null()) {
              title = body.at("title").get<std::string>();
            }
            std::optional<std::string> card_id;
            if (body.contains("card_id") && !body.at("card_id").is_null()) {
              card_id = body.at("card_id").get<std::string>();
            }
            const long long updated_at = body.at("updated_at").get<long long>();

            holder::store::AiThreadRepo repo(db_);
            if (title.has_value()) {
              repo.update_title(thread_id, title.value(), updated_at);
            } else {
              repo.touch_updated(thread_id, updated_at);
            }
            if (body.contains("card_id")) {
              repo.update_card_id(thread_id, card_id);
            }

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"thread_id", thread_id}};
            res = json_response(http::status::ok, payload);
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::delete_) {
        try {
          holder::store::AiThreadRepo repo(db_);
          repo.remove(thread_id);
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"thread_id", thread_id}};
          res = json_response(http::status::ok, payload);
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      }
    } else if (path == "/ai/messages" && req.method() == http::verb::get) {
      const std::string thread_id = param("thread_id");
      const std::string include_deleted_raw = param("include_deleted");
      if (thread_id.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing thread_id.");
      } else {
        try {
          holder::store::AiMessageRepo repo(db_, fts_);
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
            if (msg.provider.has_value()) {
              item["provider"] = msg.provider.value();
            } else {
              item["provider"] = nullptr;
            }
            if (msg.model.has_value()) {
              item["model"] = msg.model.value();
            } else {
              item["model"] = nullptr;
            }
            item["content"] = msg.content;
            item["created_at"] = msg.created_at;
            if (msg.deleted_at.has_value()) {
              item["deleted_at"] = msg.deleted_at.value();
            } else {
              item["deleted_at"] = nullptr;
            }
            if (msg.prompt_hash.has_value()) {
              item["prompt_hash"] = msg.prompt_hash.value();
            } else {
              item["prompt_hash"] = nullptr;
            }
            if (msg.meta_json.has_value()) {
              item["meta_json"] = msg.meta_json.value();
            } else {
              item["meta_json"] = nullptr;
            }
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
    } else if (path == "/ai/messages" && req.method() == http::verb::post) {
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
            msg.message_id = generate_uuid_v4();
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

          holder::store::AiMessageRepo repo(db_, fts_);
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
    } else if (path.rfind("/ai/messages/", 0) == 0) {
      const std::string rest = path.substr(std::string("/ai/messages/").size());
      const auto slash = rest.find('/');
      if (slash != std::string::npos) {
        const std::string message_id = rest.substr(0, slash);
        const std::string tail = rest.substr(slash);
        if (message_id.empty()) {
          res = error_response(http::status::not_found, "not_found", "Route not found.");
        } else if (tail == "/links") {
          try {
            holder::store::AiMessageRepo message_repo(db_, fts_);
            const auto msg_opt = message_repo.get(message_id);
            if (!msg_opt.has_value()) {
              res = error_response(http::status::not_found, "not_found", "AI message not found.");
            } else {
              holder::store::AiThreadRepo thread_repo(db_);
              const auto thread_opt = thread_repo.get(msg_opt->thread_id);
              if (!thread_opt.has_value()) {
                res = error_response(http::status::bad_request, "bad_request", "Thread not found.");
              } else {
                const std::string project_id = thread_opt->project_id;
                holder::store::LinkRepo link_repo(db_);
                if (req.method() == http::verb::get) {
                  holder::store::CardRepo card_repo(db_);
                  holder::store::AiMessageRepo msg_repo(db_, fts_);
                  const bool include_deleted = !param("include_deleted").empty() &&
                                               param("include_deleted") != "0";
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
                    if (link.label.has_value()) {
                      item["label"] = link.label.value();
                    } else {
                      item["label"] = nullptr;
                    }
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
                    if (!validate_link_target(db_,
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
                          {"created_at", link.created_at}
                      };
                      if (link.label.has_value()) {
                        payload["data"]["label"] = link.label.value();
                      } else {
                        payload["data"]["label"] = nullptr;
                      }
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
                    link_repo.delete_link(project_id,
                                          message_id,
                                          to_card_id.value(),
                                          to_type,
                                          kind);
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
            holder::store::AiMessageRepo message_repo(db_, fts_);
            const auto msg_opt = message_repo.get(message_id);
            if (!msg_opt.has_value()) {
              res = error_response(http::status::not_found, "not_found", "AI message not found.");
            } else if (req.method() != http::verb::get) {
              res = error_response(http::status::method_not_allowed,
                                   "method_not_allowed",
                                   "Method not allowed.");
            } else {
              holder::store::AiThreadRepo thread_repo(db_);
              const auto thread_opt = thread_repo.get(msg_opt->thread_id);
              if (!thread_opt.has_value()) {
                res = error_response(http::status::bad_request, "bad_request", "Thread not found.");
              } else {
                const std::string project_id = thread_opt->project_id;
                holder::store::LinkRepo link_repo(db_);
                holder::store::CardRepo card_repo(db_);
                holder::store::AiMessageRepo msg_repo(db_, fts_);
                const bool include_deleted = !param("include_deleted").empty() &&
                                             param("include_deleted") != "0";
                const auto links = link_repo.list_backlinks_typed(project_id,
                                                                  message_id,
                                                                  "ai_message");
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
                  if (link.label.has_value()) {
                    item["label"] = link.label.value();
                  } else {
                    item["label"] = nullptr;
                  }
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
              holder::store::AiMessageRepo repo(db_, fts_);
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
            holder::store::AiMessageRepo repo(db_, fts_);
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
            holder::store::AiMessageRepo repo(db_, fts_);
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
            holder::store::AiMessageRepo repo(db_, fts_);
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
              if (msg.provider.has_value()) {
                data["provider"] = msg.provider.value();
              } else {
                data["provider"] = nullptr;
              }
              if (msg.model.has_value()) {
                data["model"] = msg.model.value();
              } else {
                data["model"] = nullptr;
              }
              data["content"] = msg.content;
              data["created_at"] = msg.created_at;
              if (msg.prompt_hash.has_value()) {
                data["prompt_hash"] = msg.prompt_hash.value();
              } else {
                data["prompt_hash"] = nullptr;
              }
              if (msg.meta_json.has_value()) {
                data["meta_json"] = msg.meta_json.value();
              } else {
                data["meta_json"] = nullptr;
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
        }
      }
    } else if (path == "/resources" && req.method() == http::verb::get) {
      const std::string project_id = param("project_id");
      if (project_id.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      } else {
        try {
          holder::store::ResourceRepo repo(db_);
          const auto resources = repo.list(project_id);
          nlohmann::json data = nlohmann::json::array();
          for (const auto& resource : resources) {
            nlohmann::json item;
            item["resource_id"] = resource.resource_id;
            item["project_id"] = resource.project_id;
            item["kind"] = resource.kind;
            item["uri"] = resource.uri;
            item["label"] = resource.label;
            if (resource.desc.has_value()) {
              item["desc"] = resource.desc.value();
            } else {
              item["desc"] = nullptr;
            }
            item["created_at"] = resource.created_at;
            item["updated_at"] = resource.updated_at;
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
    } else if (path == "/resources" && req.method() == http::verb::post) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("project_id") || !body.contains("kind") || !body.contains("uri") ||
            !body.contains("label")) {
          res = error_response(http::status::bad_request, "bad_request", "Missing required fields.");
        } else {
          holder::model::Resource resource;
          if (body.contains("resource_id") && !body.at("resource_id").is_null()) {
            resource.resource_id = body.at("resource_id").get<std::string>();
          }
          if (resource.resource_id.empty()) {
            resource.resource_id = generate_uuid_v4();
          }
          resource.project_id = body.at("project_id").get<std::string>();
          resource.kind = body.at("kind").get<std::string>();
          resource.uri = body.at("uri").get<std::string>();
          resource.label = body.at("label").get<std::string>();
          if (body.contains("desc") && !body.at("desc").is_null()) {
            resource.desc = body.at("desc").get<std::string>();
          }
          if (body.contains("created_at") && !body.at("created_at").is_null()) {
            resource.created_at = body.at("created_at").get<long long>();
          }
          if (body.contains("updated_at") && !body.at("updated_at").is_null()) {
            resource.updated_at = body.at("updated_at").get<long long>();
          }
          if (resource.created_at <= 0) {
            resource.created_at = now_epoch_seconds();
          }
          if (resource.updated_at <= 0) {
            resource.updated_at = resource.created_at;
          }

          holder::store::ResourceRepo repo(db_);
          repo.add(resource);

          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"resource_id", resource.resource_id}};
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
    } else if (path.rfind("/resources/", 0) == 0) {
      const std::string resource_id = path.substr(std::string("/resources/").size());
      if (resource_id.empty()) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else if (req.method() == http::verb::patch) {
        try {
          const auto body = nlohmann::json::parse(req.body());
          if (!body.contains("updated_at")) {
            res = error_response(http::status::bad_request, "bad_request", "Missing updated_at.");
          } else {
            holder::store::ResourceRepo repo(db_);
            const auto existing = repo.get(resource_id);
            if (!existing.has_value()) {
              res = error_response(http::status::not_found, "not_found", "Resource not found.");
            } else {
              auto resource = existing.value();
              if (body.contains("kind") && !body.at("kind").is_null()) {
                resource.kind = body.at("kind").get<std::string>();
              }
              if (body.contains("uri") && !body.at("uri").is_null()) {
                resource.uri = body.at("uri").get<std::string>();
              }
              if (body.contains("label") && !body.at("label").is_null()) {
                resource.label = body.at("label").get<std::string>();
              }
              if (body.contains("desc")) {
                if (body.at("desc").is_null()) {
                  resource.desc.reset();
                } else {
                  resource.desc = body.at("desc").get<std::string>();
                }
              }
              resource.updated_at = body.at("updated_at").get<long long>();
              repo.update(resource);

              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"resource_id", resource_id}};
              res = json_response(http::status::ok, payload);
            }
          }
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else if (req.method() == http::verb::delete_) {
        try {
          holder::store::ResourceRepo repo(db_);
          repo.remove(resource_id);
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"resource_id", resource_id}};
          res = json_response(http::status::ok, payload);
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      } else {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      }
    } else if (path == "/trash" && req.method() == http::verb::get) {
      const std::string project_id = param("project_id");
      const std::string type = param("type");
      if (project_id.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      } else {
        try {
          nlohmann::json data = nlohmann::json::array();
          if (type.empty() || type == "card" || type == "all") {
            holder::store::CardRepo card_repo(db_);
            const auto cards = card_repo.list(project_id, std::nullopt);
            for (const auto& card : cards) {
              if (!card.deleted_at.has_value()) continue;
              nlohmann::json item;
              item["type"] = "card";
              item["card_id"] = card.card_id;
              item["project_id"] = card.project_id;
              item["title"] = card.title;
              item["deleted_at"] = card.deleted_at.value();
              item["rel_path"] = card.rel_path;
              data.push_back(std::move(item));
            }
          }
          if (type.empty() || type == "ai_message" || type == "all") {
            holder::store::AiMessageRepo msg_repo(db_, fts_);
            const auto msgs = msg_repo.list_deleted_by_project(project_id);
            for (const auto& msg : msgs) {
              if (!msg.deleted_at.has_value()) continue;
              nlohmann::json item;
              item["type"] = "ai_message";
              item["message_id"] = msg.message_id;
              item["thread_id"] = msg.thread_id;
              item["project_id"] = project_id;
              item["role"] = msg.role;
              item["deleted_at"] = msg.deleted_at.value();
              item["created_at"] = msg.created_at;
              data.push_back(std::move(item));
            }
          }
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = json_response(http::status::ok, payload);
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    } else if (path == "/trash" && req.method() == http::verb::delete_) {
      const std::string project_id = param("project_id");
      const std::string type = param("type");
      if (project_id.empty()) {
        res = error_response(http::status::bad_request, "bad_request", "Missing project_id.");
      } else {
        try {
          if ((type.empty() || type == "card" || type == "all") && card_store_) {
            holder::store::CardRepo card_repo(db_);
            const auto cards = card_repo.list(project_id, std::nullopt);
            for (const auto& card : cards) {
              if (!card.deleted_at.has_value()) continue;
              card_store_->hard_delete(card.card_id);
            }
          }
          if (type.empty() || type == "ai_message" || type == "all") {
            holder::store::AiMessageRepo msg_repo(db_, fts_);
            const auto msgs = msg_repo.list_deleted_by_project(project_id);
            for (const auto& msg : msgs) {
              msg_repo.remove(msg.message_id);
            }
          }
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"project_id", project_id}};
          res = json_response(http::status::ok, payload);
        } catch (const std::exception& ex) {
          res = error_response(http::status::bad_request, "bad_request", ex.what());
        }
      }
    } else if (path.rfind("/trash/", 0) == 0 && req.method() == http::verb::delete_) {
      const std::string rest = path.substr(std::string("/trash/").size());
      const auto slash = rest.find('/');
      if (slash == std::string::npos) {
        res = error_response(http::status::not_found, "not_found", "Route not found.");
      } else {
        const std::string type = rest.substr(0, slash);
        const std::string id = rest.substr(slash + 1);
        if (id.empty()) {
          res = error_response(http::status::bad_request, "bad_request", "Missing id.");
        } else {
          try {
            if (type == "card") {
              if (!card_store_) {
                res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
              } else {
                card_store_->hard_delete(id);
                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = {{"card_id", id}};
                res = json_response(http::status::ok, payload);
              }
            } else if (type == "ai_message") {
              holder::store::AiMessageRepo msg_repo(db_, fts_);
              msg_repo.remove(id);
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"message_id", id}};
              res = json_response(http::status::ok, payload);
            } else {
              res = error_response(http::status::bad_request, "bad_request", "Unknown trash type.");
            }
          } catch (const std::exception& ex) {
            res = error_response(http::status::bad_request, "bad_request", ex.what());
          }
        }
      }
    } else if (path == "/cards" && req.method() == http::verb::get) {
      const std::string project_id = param("project_id");
      const std::string parent_raw = param("parent_card_id");
      const std::string include_deleted_raw = param("include_deleted");
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
      const std::string rest = path.substr(std::string("/cards/").size());
      const auto slash = rest.find('/');
      if (slash != std::string::npos) {
        const std::string card_id = rest.substr(0, slash);
        const std::string tail = rest.substr(slash);
        if (card_id.empty()) {
          res = error_response(http::status::not_found, "not_found", "Route not found.");
        } else if (!card_store_) {
          res = error_response(http::status::not_implemented, "not_implemented", "Card store unavailable.");
        } else if (tail == "/links") {
          try {
            const auto card_opt = card_store_->get(card_id);
            if (!card_opt.has_value()) {
              res = error_response(http::status::not_found, "not_found", "Card not found.");
            } else {
              const auto& card = card_opt.value();
              holder::store::LinkRepo repo(db_);
              if (req.method() == http::verb::get) {
                holder::store::CardRepo card_repo(db_);
                holder::store::AiMessageRepo msg_repo(db_, fts_);
                const bool include_deleted = !param("include_deleted").empty() &&
                                             param("include_deleted") != "0";
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
                  if (link.label.has_value()) {
                    item["label"] = link.label.value();
                  } else {
                    item["label"] = nullptr;
                  }
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
                    link.created_at = now_epoch_seconds();
                  }
                  std::string validation_error;
                  if (!validate_link_target(db_,
                                            card.project_id,
                                            link.to_card_id,
                                            link.to_type,
                                            validation_error)) {
                    res = error_response(http::status::bad_request, "bad_request", validation_error);
                  } else {
                    repo.upsert_links(card.project_id, card.card_id, {link});
                    card_store_->update_links(card.card_id, now_epoch_seconds());

                    nlohmann::json payload;
                    payload["ok"] = true;
                    payload["data"] = {
                        {"from_card_id", link.from_card_id},
                        {"to_card_id", link.to_card_id},
                        {"to_type", link.to_type},
                        {"kind", link.kind},
                        {"created_at", link.created_at}
                    };
                    if (link.label.has_value()) {
                      payload["data"]["label"] = link.label.value();
                    } else {
                      payload["data"]["label"] = nullptr;
                    }
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
                  repo.delete_link(card.project_id, card.card_id, to_card_id.value(), to_type, kind);
                } else {
                  repo.delete_links_from(card.project_id, card.card_id);
                }
                card_store_->update_links(card.card_id, now_epoch_seconds());

                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = {{"card_id", card.card_id}};
                res = json_response(http::status::ok, payload);
              } else {
                res = error_response(http::status::method_not_allowed,
                                     "method_not_allowed",
                                     "Method not allowed.");
              }
            }
          } catch (const std::exception& ex) {
            res = error_response(http::status::bad_request, "bad_request", ex.what());
          }
        } else if (tail == "/backlinks") {
          try {
            const auto card_opt = card_store_->get(card_id);
            if (!card_opt.has_value()) {
              res = error_response(http::status::not_found, "not_found", "Card not found.");
            } else if (req.method() != http::verb::get) {
              res = error_response(http::status::method_not_allowed,
                                   "method_not_allowed",
                                   "Method not allowed.");
            } else {
              const auto& card = card_opt.value();
              holder::store::LinkRepo repo(db_);
              holder::store::CardRepo card_repo(db_);
              holder::store::AiMessageRepo msg_repo(db_, fts_);
              const bool include_deleted = !param("include_deleted").empty() &&
                                           param("include_deleted") != "0";
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
                if (link.label.has_value()) {
                  item["label"] = link.label.value();
                } else {
                  item["label"] = nullptr;
                }
                data.push_back(std::move(item));
              }
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = data;
              res = json_response(http::status::ok, payload);
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
              const long long updated_at = now_epoch_seconds();
              card_store_->restore(card_id, updated_at);
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"card_id", card_id}};
              res = json_response(http::status::ok, payload);
            } catch (const std::exception& ex) {
              res = error_response(http::status::bad_request, "bad_request", ex.what());
            }
          }
        } else {
          res = error_response(http::status::not_found, "not_found", "Route not found.");
        }
      } else {
        const std::string card_id = rest;
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
              if (card.deleted_at.has_value()) {
                res = error_response(http::status::not_found, "not_found", "Card not found.");
              } else {
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
        } else if (req.method() == http::verb::delete_) {
          try {
            const long long deleted_at = now_epoch_seconds();
            card_store_->trash(card_id, deleted_at);
            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"card_id", card_id}};
            res = json_response(http::status::ok, payload);
          } catch (const std::exception& ex) {
            res = error_response(http::status::bad_request, "bad_request", ex.what());
          }
        } else if (req.method() == http::verb::post && rest.size() > 0) {
          res = error_response(http::status::not_found, "not_found", "Route not found.");
        } else {
          res = error_response(http::status::not_found, "not_found", "Route not found.");
        }
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
