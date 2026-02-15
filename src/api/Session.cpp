#include "api/Session.h"
#include "api/routes/AiMessageRoutes.h"
#include "api/routes/AiResourceRoutes.h"
#include "api/routes/AiRunnerRoutes.h"
#include "api/routes/AiThreadRoutes.h"
#include "api/routes/CardRoutes.h"
#include "api/routes/TrashRoutes.h"
#include "api/routes/StaticRoutes.h"
#include "api/routes/AiRunRoutes.h"
#include "api/routes/AiStatusRoutes.h"
#include "api/routes/AiProviderRoutes.h"
#include "api/support/PathDiscovery.h"
#include "api/support/CloudConfig.h"
#include "api/support/CloudClient.h"
#include "api/support/CloudQuota.h"
#include "api/support/LocalModelRouting.h"
#include "api/support/RunEventStore.h"

#include "caste.hpp"
#include "core/CardPaths.h"
#include "core/ProjectPaths.h"
#include "git/GitOps.h"
#include "llm/LocalModelRunner.h"
#include "core/ServerInfo.h"
#include "store/AiProviderCredentialRepo.h"
#include "store/AiRouterConfigRepo.h"
#include "store/AiRunRepo.h"
#include "store/ProjectRepo.h"
#include "store/Rebuilder.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <string>
#include <thread>
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

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
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

  if (routes::handle_static_routes(path, req, res)) {
    // handled
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
    } else if (routes::handle_ai_status_routes(path, req, res, db_, runner_, param)) {
      // handled
    } else if (routes::handle_ai_provider_routes(path, req, res, db_)) {
      // handled
    } else if (const auto route_result = routes::handle_ai_run_routes(path,
                                                                      req,
                                                                      res,
                                                                      socket_,
                                                                      db_,
                                                                      fts_,
                                                                      runner_,
                                                                      [&]() { return generate_uuid_v4(); },
                                                                      param);
               route_result.handled) {
      if (route_result.streamed) return;
    } else if (const auto runner_route_result =
                   routes::handle_ai_runner_routes(path, req, res, socket_, runner_);
               runner_route_result.handled) {
      if (runner_route_result.streamed) return;
    } else if (routes::handle_ai_thread_routes(path,
                                               req,
                                               res,
                                               db_,
                                               [&]() { return generate_uuid_v4(); },
                                               param)) {
      // handled
    } else if (routes::handle_ai_message_routes(path,
                                                req,
                                                res,
                                                db_,
                                                fts_,
                                                [&]() { return generate_uuid_v4(); },
                                                param)) {
      // handled
    } else if (routes::handle_ai_resource_routes(path,
                                                 req,
                                                 res,
                                                 db_,
                                                 [&]() { return generate_uuid_v4(); },
                                                 param)) {
      // handled
    } else if (routes::handle_trash_routes(path, req, res, db_, card_store_, fts_, param)) {
      // handled
    } else if (routes::handle_card_routes(path,
                                          req,
                                          res,
                                          db_,
                                          card_store_,
                                          fts_,
                                          [&]() { return generate_uuid_v4(); },
                                          param)) {
      // handled
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
