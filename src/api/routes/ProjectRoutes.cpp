#include "api/routes/ProjectRoutes.h"
#include "api/support/HttpResponses.h"

#include "core/ProjectPaths.h"
#include "git/GitRepo.h"
#include "store/ProjectRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

long long now_epoch_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
}

} // namespace

bool handle_project_routes(const std::string& path,
                           const http::request<http::string_body>& req,
                           http::response<http::string_body>& res,
                           holder::store::Db& db,
                           holder::git::GitOps* git_ops,
                           const std::function<std::string()>& uuid_v4,
                           const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/projects" && req.method() == http::verb::get) {
    try {
      holder::store::ProjectRepo repo(db);
      auto projects = repo.list();
      const std::string name_filter = param_get("name");
      const std::string updated_after_raw = param_get("updated_after");
      const std::string updated_before_raw = param_get("updated_before");
      const std::string limit_raw = param_get("limit");
      const std::string offset_raw = param_get("offset");
      const std::string order_raw = param_get("order");
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

      std::stable_sort(projects.begin(),
                       projects.end(),
                       [order_asc, order_key](const holder::model::Project& a,
                                              const holder::model::Project& b) {
                         switch (order_key) {
                           case OrderKey::CreatedAt:
                             return order_asc ? (a.created_at < b.created_at)
                                              : (a.created_at > b.created_at);
                           case OrderKey::Name:
                             return order_asc ? (a.name < b.name) : (a.name > b.name);
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
          const std::size_t end =
              std::min(projects.size(), start + static_cast<std::size_t>(limit));
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
            {"git_remote_url", project.git_remote_url.has_value()
                                   ? nlohmann::json(project.git_remote_url.value())
                                   : nlohmann::json(nullptr)},
            {"git_provider", project.git_provider.has_value()
                                 ? nlohmann::json(project.git_provider.value())
                                 : nlohmann::json(nullptr)},
            {"created_at", project.created_at},
            {"updated_at", project.updated_at},
        });
      }
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = data;
      res = support::json_response(http::status::ok, payload);
    } catch (const std::invalid_argument&) {
      res = support::error_response(http::status::bad_request, "bad_request", "Invalid filter value.");
    } catch (const std::out_of_range&) {
      res = support::error_response(http::status::bad_request, "bad_request", "Filter value out of range.");
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::internal_server_error, "error", ex.what());
    }
    return true;
  }

  if (path == "/projects" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("name")) {
        res = support::error_response(http::status::bad_request, "bad_request", "Missing required fields.");
      } else {
        holder::model::Project project;
        if (body.contains("project_id") && !body.at("project_id").is_null()) {
          project.project_id = body.at("project_id").get<std::string>();
        }
        if (project.project_id.empty()) {
          project.project_id = uuid_v4();
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

        holder::store::ProjectRepo repo(db);
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
        res = support::json_response(http::status::created, payload);
      }
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path.rfind("/projects/", 0) == 0) {
    const std::string project_id = path.substr(std::string("/projects/").size());
    if (project_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    } else if (req.method() == http::verb::get) {
      try {
        holder::store::ProjectRepo repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found, "not_found", "Project not found.");
        } else {
          const auto& project = project_opt.value();
          nlohmann::json data;
          data["project_id"] = project.project_id;
          data["name"] = project.name;
          data["root_path"] = project.root_path;
          data["git_remote_url"] = project.git_remote_url.has_value()
                                       ? nlohmann::json(project.git_remote_url.value())
                                       : nlohmann::json(nullptr);
          data["git_provider"] = project.git_provider.has_value()
                                     ? nlohmann::json(project.git_provider.value())
                                     : nlohmann::json(nullptr);
          data["created_at"] = project.created_at;
          data["updated_at"] = project.updated_at;

          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = support::json_response(http::status::ok, payload);
        }
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::internal_server_error, "error", ex.what());
      }
    } else if (req.method() == http::verb::patch) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("updated_at")) {
          res = support::error_response(http::status::bad_request, "bad_request", "Missing required fields.");
        } else {
          const long long updated_at = body.at("updated_at").get<long long>();
          const bool has_name = body.contains("name") && !body.at("name").is_null();
          const bool has_root = body.contains("root_path") && !body.at("root_path").is_null();
          const bool has_git_remote = body.contains("git_remote_url");
          const bool has_git_provider = body.contains("git_provider");
          if (!has_name && !has_root && !has_git_remote && !has_git_provider) {
            res = support::error_response(http::status::bad_request, "bad_request", "No fields to update.");
          } else {
            holder::store::ProjectRepo repo(db);
            const auto project_opt = repo.get(project_id);
            if (!project_opt.has_value()) {
              res = support::error_response(http::status::not_found, "not_found", "Project not found.");
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
                  repo.update_git_remote(project_id,
                                         std::optional<std::string>(
                                             body.at("git_remote_url").get<std::string>()),
                                         updated_at);
                }
              }
              if (has_git_provider) {
                if (body.at("git_provider").is_null()) {
                  repo.update_git_provider(project_id, std::nullopt, updated_at);
                } else {
                  repo.update_git_provider(project_id,
                                           std::optional<std::string>(
                                               body.at("git_provider").get<std::string>()),
                                           updated_at);
                }
              }
              if (has_git_remote) {
                const std::string repo_root =
                    has_root ? body.at("root_path").get<std::string>() : project_opt->root_path;
                auto& git = resolve_git(git_ops);
                git.open_or_init(repo_root);
                if (body.at("git_remote_url").is_null()) {
                  git.remove_remote("origin");
                } else {
                  git.set_remote("origin", body.at("git_remote_url").get<std::string>());
                }
              }
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"project_id", project_id}};
              res = support::json_response(http::status::ok, payload);
            }
          }
        }
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (req.method() == http::verb::delete_) {
      try {
        holder::store::ProjectRepo repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found, "not_found", "Project not found.");
        } else {
          repo.remove(project_id);
          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = {{"project_id", project_id}};
          res = support::json_response(http::status::ok, payload);
        }
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::internal_server_error, "error", ex.what());
      }
    } else {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
