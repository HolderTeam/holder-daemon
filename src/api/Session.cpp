#include "api/Session.h"
#include "api/routes/AiMessageRoutes.h"
#include "api/routes/AiResourceRoutes.h"
#include "api/routes/AiRunnerRoutes.h"
#include "api/routes/AiThreadRoutes.h"
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
#include "store/AiThreadRepo.h"
#include "store/CardRepo.h"
#include "store/AiMessageRepo.h"
#include "store/AiProviderCredentialRepo.h"
#include "store/AiRouterConfigRepo.h"
#include "store/AiRunRepo.h"
#include "store/LinkRepo.h"
#include "store/ProjectRepo.h"
#include "store/Rebuilder.h"
#include "store/ResourceRepo.h"

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
