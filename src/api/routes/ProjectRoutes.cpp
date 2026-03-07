#include "api/routes/ProjectRoutes.h"
#include "api/support/HttpResponses.h"
#include "api/support/Time.h"

#include "core/ProjectPaths.h"
#include "git/RepoSyncMetrics.h"
#include "privacy/ProjectPrivacy.h"
#include "store/CardRepo.h"
#include "store/ProjectRepo.h"
#include "store/ProjectSyncRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

holder::git::GitOps& resolve_git(holder::git::GitOps* git) {
  static holder::git::RealGitOps real_git;
  return git ? *git : real_git;
}

bool is_valid_privacy_mode(const std::string& mode) {
  return mode == "encrypted_git" || mode == "plain";
}

bool is_home_project_name(const std::string& name) {
  std::string lowered;
  lowered.reserve(name.size());
  for (const char c : name) {
    lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return lowered == "home";
}

std::optional<bool> parse_count_param(const std::string& count_raw) {
  if (count_raw.empty()) return false;
  if (count_raw == "1" || count_raw == "true") return true;
  if (count_raw == "0" || count_raw == "false") return false;
  return std::nullopt;
}

nlohmann::json git_test_remote_payload(const std::string& project_id,
                                       const std::optional<std::string>& remote_url,
                                       const std::string& branch,
                                       holder::git::RemoteProbeStatus status,
                                       bool remote_has_head,
                                       const std::optional<std::string>& error_message) {
  const bool reachable = status == holder::git::RemoteProbeStatus::Reachable;
  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = {
      {"project_id", project_id},
      {"remote_url", remote_url.has_value() ? nlohmann::json(remote_url.value())
                                            : nlohmann::json(nullptr)},
      {"branch", branch},
      {"status", holder::git::remote_probe_status_name(status)},
      {"remote_has_head", remote_has_head},
      {"error_code", reachable ? nlohmann::json(nullptr)
                               : nlohmann::json(holder::git::remote_probe_status_name(status))},
      {"error_message", error_message.has_value() && !error_message->empty()
                            ? nlohmann::json(error_message.value())
                            : nlohmann::json(nullptr)},
  };
  return payload;
}

nlohmann::json git_push_payload(const std::string& project_id,
                                const std::optional<std::string>& remote_url,
                                const std::string& branch,
                                holder::git::PushStatus status,
                                int ahead_count,
                                int behind_count,
                                const std::optional<std::string>& error_message) {
  const bool ok = status == holder::git::PushStatus::Pushed ||
                  status == holder::git::PushStatus::UpToDate;
  std::optional<std::string> next_action;
  switch (status) {
    case holder::git::PushStatus::NonFastForward:
      next_action = "pull_then_retry";
      break;
    case holder::git::PushStatus::AuthFailed:
      next_action = "fix_auth_then_retry";
      break;
    case holder::git::PushStatus::NotFound:
      next_action = "fix_remote_url_then_retry";
      break;
    case holder::git::PushStatus::NetworkError:
    case holder::git::PushStatus::UnknownError:
      next_action = "retry";
      break;
    default:
      break;
  }

  nlohmann::json payload;
  payload["ok"] = true;
  payload["data"] = {
      {"project_id", project_id},
      {"remote_url", remote_url.has_value() ? nlohmann::json(remote_url.value())
                                            : nlohmann::json(nullptr)},
      {"branch", branch},
      {"status", holder::git::push_status_name(status)},
      {"ahead_count", ahead_count},
      {"behind_count", behind_count},
      {"error_code", ok ? nlohmann::json(nullptr)
                        : nlohmann::json(holder::git::push_status_name(status))},
      {"error_message", error_message.has_value() && !error_message->empty()
                            ? nlohmann::json(error_message.value())
                            : nlohmann::json(nullptr)},
      {"next_action", next_action.has_value() ? nlohmann::json(next_action.value())
                                              : nlohmann::json(nullptr)},
  };
  return payload;
}

nlohmann::json project_sync_to_json(const std::optional<holder::model::ProjectSyncState>& sync_opt) {
  const auto as_json_or_null = [](const auto& value) -> nlohmann::json {
    return value.has_value() ? nlohmann::json(value.value()) : nlohmann::json(nullptr);
  };

  if (!sync_opt.has_value()) {
    return {
        {"last_commit_at", nullptr},
        {"last_push_at", nullptr},
        {"last_pull_at", nullptr},
        {"uncommitted_changes_count", 0},
        {"unpushed_commits_count", 0},
        {"last_push_status", nullptr},
        {"last_pull_status", nullptr},
        {"last_sync_error", nullptr},
        {"last_sync_error_at", nullptr},
        {"retry_count", 0},
        {"next_retry_at", nullptr},
        {"pull_retry_count", 0},
        {"next_pull_retry_at", nullptr},
        {"updated_at", nullptr},
    };
  }

  const auto& sync = sync_opt.value();
  return {
      {"last_commit_at", as_json_or_null(sync.last_commit_at)},
      {"last_push_at", as_json_or_null(sync.last_push_at)},
      {"last_pull_at", as_json_or_null(sync.last_pull_at)},
      {"uncommitted_changes_count", sync.uncommitted_changes_count},
      {"unpushed_commits_count", sync.unpushed_commits_count},
      {"last_push_status", as_json_or_null(sync.last_push_status)},
      {"last_pull_status", as_json_or_null(sync.last_pull_status)},
      {"last_sync_error", as_json_or_null(sync.last_sync_error)},
      {"last_sync_error_at", as_json_or_null(sync.last_sync_error_at)},
      {"retry_count", sync.retry_count},
      {"next_retry_at", as_json_or_null(sync.next_retry_at)},
      {"pull_retry_count", sync.pull_retry_count},
      {"next_pull_retry_at", as_json_or_null(sync.next_pull_retry_at)},
      {"updated_at", sync.updated_at > 0 ? nlohmann::json(sync.updated_at) : nlohmann::json(nullptr)},
  };
}

http::response<http::string_body> privacy_error_response(
    const holder::privacy::PrivacyError& ex) {
  http::status status = http::status::bad_request;
  switch (ex.code()) {
    case holder::privacy::PrivacyErrorCode::KeyringUnavailable:
      status = http::status::service_unavailable;
      break;
    case holder::privacy::PrivacyErrorCode::KeyMaterialMissing:
    case holder::privacy::PrivacyErrorCode::CryptMetadataMissing:
      status = http::status::internal_server_error;
      break;
    case holder::privacy::PrivacyErrorCode::RecoveryTokenInvalid:
    case holder::privacy::PrivacyErrorCode::EnvelopeInvalid:
    case holder::privacy::PrivacyErrorCode::EnvelopeMetadataMismatch:
    case holder::privacy::PrivacyErrorCode::EncryptionSafetyCheckFailed:
    case holder::privacy::PrivacyErrorCode::PrivacyCryptoFailed:
      status = http::status::bad_request;
      break;
  }
  return support::error_response(status,
                                 holder::privacy::privacy_error_code_name(ex.code()),
                                 ex.what());
}

} // namespace

bool handle_project_routes(const std::string& path,
                           const http::request<http::string_body>& req,
                           http::response<http::string_body>& res,
                           holder::store::Db& db,
                           holder::git::GitOps* git_ops,
                           const std::function<std::string()>& uuid_v4,
                           const std::function<std::string(const std::string&)>& param_get) {
  if (path == "/recovery-token/import" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("pin") || body.at("pin").is_null() ||
          !body.contains("recovery_token") || body.at("recovery_token").is_null()) {
        res = support::error_response(http::status::bad_request,
                                      "bad_request",
                                      "Missing required fields.");
        return true;
      }

      const std::string pin = body.at("pin").get<std::string>();
      const std::string recovery_token = body.at("recovery_token").get<std::string>();
      const auto metadata = holder::privacy::inspect_recovery_token(pin, recovery_token);

      holder::store::ProjectRepo repo(db);
      holder::store::ProjectSyncRepo sync_repo(db);
      const long long now = support::now_epoch_seconds();
      bool project_created = false;
      auto project_opt = repo.get(metadata.project_id);
      if (!project_opt.has_value()) {
        holder::model::Project project;
        project.project_id = metadata.project_id;
        project.name = metadata.project_name.has_value() && !metadata.project_name->empty()
                           ? metadata.project_name.value()
                           : "Recovered Project";
        project.privacy_mode = "encrypted_git";
        project.created_at = now;
        project.updated_at = now;
        const auto base_root = holder::core::default_projects_root();
        const auto slug = holder::core::slugify(project.name);
        project.root_path = holder::core::unique_project_root(base_root, slug, repo.list());
        repo.create(project);
        project_created = true;
        project_opt = repo.get(metadata.project_id);
      }

      holder::privacy::import_recovery_token(
          repo,
          metadata.project_id,
          pin,
          recovery_token,
          now);

      const bool remote_hint_present =
          metadata.git_remote_url.has_value() && !metadata.git_remote_url->empty();
      bool remote_configured = false;
      std::string pull_status = "not_attempted";
      std::string remote_error;
      std::string pull_error;
      if (remote_hint_present) {
        auto& git = resolve_git(git_ops);
        const auto refreshed = repo.get(metadata.project_id);
        if (refreshed.has_value()) {
          git.open_or_init(refreshed->root_path);
          try {
            git.set_remote("origin", metadata.git_remote_url.value());
            remote_configured = true;
          } catch (const std::exception& ex) {
            remote_error = ex.what();
          }

          if (remote_configured) {
            try {
              git.pull_remote_ff_only("origin");
              pull_status = "succeeded";
              sync_repo.record_pull_result(
                  metadata.project_id,
                  pull_status,
                  true,
                  std::nullopt,
                  now);
            } catch (const std::exception& ex) {
              pull_status = "failed";
              pull_error = ex.what();
              sync_repo.record_pull_result(
                  metadata.project_id,
                  pull_status,
                  false,
                  pull_error,
                  now);
            }
          }
          try {
            const auto metrics = holder::git::inspect_repo_sync_metrics(refreshed->root_path, "origin");
            sync_repo.update_activity_counts(
                metadata.project_id,
                metrics.uncommitted_changes_count,
                metrics.unpushed_commits_count,
                now);
          } catch (const std::exception&) {
            // Best-effort only.
          }
        }
      }

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {
          {"project_id", metadata.project_id},
          {"project_created", project_created},
          {"remote_hint_present", remote_hint_present},
          {"remote_configured", remote_configured},
          {"remote_error", remote_error.empty() ? nlohmann::json(nullptr)
                                                : nlohmann::json(remote_error)},
          {"pull_status", pull_status},
          {"pull_error", pull_error.empty() ? nlohmann::json(nullptr)
                                            : nlohmann::json(pull_error)},
      };
      res = support::json_response(project_created ? http::status::created : http::status::ok,
                                   payload);
    } catch (const holder::privacy::PrivacyError& ex) {
      res = privacy_error_response(ex);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path == "/projects" && req.method() == http::verb::get) {
    try {
      holder::store::ProjectRepo repo(db);
      holder::store::ProjectSyncRepo sync_repo(db);
      auto projects = repo.list();
      const std::string name_filter = param_get("name");
      const std::string updated_after_raw = param_get("updated_after");
      const std::string updated_before_raw = param_get("updated_before");
      const std::string limit_raw = param_get("limit");
      const std::string offset_raw = param_get("offset");
      const std::string order_raw = param_get("order");
      const std::string count_raw = param_get("count");
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
      const auto include_count = parse_count_param(count_raw);
      if (!include_count.has_value()) {
        throw std::invalid_argument("invalid count");
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

      if (order_raw.empty()) {
        std::stable_sort(projects.begin(),
                         projects.end(),
                         [](const holder::model::Project& a, const holder::model::Project& b) {
                           const bool a_home = is_home_project_name(a.name);
                           const bool b_home = is_home_project_name(b.name);
                           if (a_home != b_home) return a_home;
                           return false;
                         });
      }

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
      holder::store::CardRepo card_repo(db);
      for (const auto& project : projects) {
        nlohmann::json item = {
            {"project_id", project.project_id},
            {"name", project.name},
            {"root_path", project.root_path},
            {"git_remote_url", project.git_remote_url.has_value()
                                   ? nlohmann::json(project.git_remote_url.value())
                                   : nlohmann::json(nullptr)},
            {"git_provider", project.git_provider.has_value()
                                 ? nlohmann::json(project.git_provider.value())
                                 : nlohmann::json(nullptr)},
            {"privacy_mode", project.privacy_mode},
            {"project_key_id", project.project_key_id.has_value()
                                   ? nlohmann::json(project.project_key_id.value())
                                   : nlohmann::json(nullptr)},
            {"created_at", project.created_at},
            {"updated_at", project.updated_at},
            {"sync", project_sync_to_json(sync_repo.get(project.project_id))},
        };
        if (include_count.value()) {
          item["card_count"] = card_repo.count_all_not_deleted(project.project_id);
          item["root_card_count"] = card_repo.count_roots_not_deleted(project.project_id);
        }
        data.push_back(std::move(item));
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
        if (body.contains("privacy_mode") && !body.at("privacy_mode").is_null()) {
          project.privacy_mode = body.at("privacy_mode").get<std::string>();
        }
        if (!is_valid_privacy_mode(project.privacy_mode)) {
          res = support::error_response(http::status::bad_request,
                                        "bad_request",
                                        "privacy_mode must be encrypted_git or plain.");
          return true;
        }
        if (body.contains("project_key_id")) {
          if (body.at("project_key_id").is_null()) {
            project.project_key_id.reset();
          } else {
            project.project_key_id = body.at("project_key_id").get<std::string>();
          }
        }
        if (project.created_at <= 0) {
          project.created_at = support::now_epoch_seconds();
        }
        if (project.updated_at <= 0) {
          project.updated_at = project.created_at;
        }

        holder::store::ProjectRepo repo(db);
        holder::store::ProjectSyncRepo sync_repo(db);
        if (project.root_path.empty()) {
          const auto base_root = holder::core::default_projects_root();
          const auto slug = holder::core::slugify(project.name);
          project.root_path = holder::core::unique_project_root(base_root, slug, repo.list());
        }
        repo.create(project);

        auto& git = resolve_git(git_ops);
        if (project.git_remote_url.has_value()) {
          git.open_or_init(project.root_path);
          git.set_remote("origin", project.git_remote_url.value());
        }
        if (project.privacy_mode == "encrypted_git") {
          holder::privacy::ensure_encrypted_project_ready(
              git,
              repo,
              project.project_id,
              project.root_path,
              project.project_key_id,
              project.updated_at,
              uuid_v4);
        }
        if (const auto persisted = repo.get(project.project_id); persisted.has_value()) {
          project = persisted.value();
        }

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
        data["privacy_mode"] = project.privacy_mode;
        data["project_key_id"] = project.project_key_id.has_value()
                                     ? nlohmann::json(project.project_key_id.value())
                                     : nlohmann::json(nullptr);
        data["created_at"] = project.created_at;
        data["updated_at"] = project.updated_at;
        data["sync"] = project_sync_to_json(sync_repo.get(project.project_id));
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = support::json_response(http::status::created, payload);
      }
    } catch (const holder::privacy::PrivacyError& ex) {
      res = privacy_error_response(ex);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path.rfind("/projects/", 0) == 0) {
    const std::string suffix = path.substr(std::string("/projects/").size());
    const auto slash_pos = suffix.find('/');
    const std::string project_id = slash_pos == std::string::npos ? suffix : suffix.substr(0, slash_pos);
    const std::string subpath = slash_pos == std::string::npos ? "" : suffix.substr(slash_pos);
    if (project_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    } else if (subpath == "/git/test-remote" && req.method() == http::verb::post) {
      try {
        const auto body = req.body().empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body());
        const std::string branch =
            body.contains("branch") && !body.at("branch").is_null()
                ? body.at("branch").get<std::string>()
                : "";

        holder::store::ProjectRepo repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found, "not_found", "Project not found.");
          return true;
        }
        auto project = project_opt.value();

        std::optional<std::string> remote_url = project.git_remote_url;
        if (body.contains("remote_url")) {
          if (body.at("remote_url").is_null()) {
            remote_url = std::nullopt;
          } else {
            remote_url = body.at("remote_url").get<std::string>();
          }

          // Optional override in this call also updates persisted project remote.
          repo.update_git_remote(project_id, remote_url, support::now_epoch_seconds());
          project = repo.get(project_id).value_or(project);
        }

        if (!remote_url.has_value() || remote_url->empty()) {
          const auto payload = git_test_remote_payload(project_id,
                                                       remote_url,
                                                       branch.empty() ? "local_default" : branch,
                                                       holder::git::RemoteProbeStatus::RemoteUnset,
                                                       false,
                                                       "Remote URL is not configured.");
          res = support::json_response(http::status::ok, payload);
          return true;
        }

        auto& git = resolve_git(git_ops);
        git.open_or_init(project.root_path);
        git.set_remote("origin", remote_url.value());
        const auto probe = git.probe_remote("origin");
        const auto payload = git_test_remote_payload(project_id,
                                                     remote_url,
                                                     branch.empty() ? "local_default" : branch,
                                                     probe.status,
                                                     probe.remote_has_head,
                                                     probe.error_message.empty()
                                                         ? std::optional<std::string>()
                                                         : std::optional<std::string>(probe.error_message));
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (subpath == "/git/push" && req.method() == http::verb::post) {
      try {
        const auto body = req.body().empty() ? nlohmann::json::object() : nlohmann::json::parse(req.body());
        const std::string branch =
            body.contains("branch") && !body.at("branch").is_null()
                ? body.at("branch").get<std::string>()
                : "";
        const bool set_upstream =
            body.contains("set_upstream") && !body.at("set_upstream").is_null()
                ? body.at("set_upstream").get<bool>()
                : true;

        holder::store::ProjectRepo repo(db);
        holder::store::ProjectSyncRepo sync_repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found, "not_found", "Project not found.");
          return true;
        }
        const auto& project = project_opt.value();
        if (!project.git_remote_url.has_value() || project.git_remote_url->empty()) {
          sync_repo.record_push_result(project_id,
                                       holder::git::push_status_name(holder::git::PushStatus::RemoteUnset),
                                       false,
                                       std::optional<std::string>{"Remote URL is not configured."},
                                       support::now_epoch_seconds());
          const auto payload = git_push_payload(project_id,
                                                project.git_remote_url,
                                                branch.empty() ? "local_default" : branch,
                                                holder::git::PushStatus::RemoteUnset,
                                                0,
                                                0,
                                                "Remote URL is not configured.");
          res = support::json_response(http::status::ok, payload);
          return true;
        }

        auto& git = resolve_git(git_ops);
        git.open_or_init(project.root_path);
        git.set_remote("origin", project.git_remote_url.value());
        const auto push = git.push_branch("origin", branch, set_upstream);
        const bool push_ok = push.status == holder::git::PushStatus::Pushed ||
                             push.status == holder::git::PushStatus::UpToDate;
        sync_repo.record_push_result(
            project_id,
            holder::git::push_status_name(push.status),
            push_ok,
            push.error_message.empty() ? std::optional<std::string>()
                                       : std::optional<std::string>(push.error_message),
            support::now_epoch_seconds());
        try {
          const auto metrics = holder::git::inspect_repo_sync_metrics(project.root_path, "origin");
          sync_repo.update_activity_counts(
              project_id,
              metrics.uncommitted_changes_count,
              metrics.unpushed_commits_count,
              support::now_epoch_seconds());
        } catch (const std::exception&) {
          // Best-effort only.
        }
        const auto payload = git_push_payload(project_id,
                                              project.git_remote_url,
                                              branch.empty() ? "local_default" : branch,
                                              push.status,
                                              push.ahead_count,
                                              push.behind_count,
                                              push.error_message.empty()
                                                  ? std::optional<std::string>()
                                                  : std::optional<std::string>(push.error_message));
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (subpath == "/git/sync-status" && req.method() == http::verb::get) {
      try {
        holder::store::ProjectRepo repo(db);
        holder::store::ProjectSyncRepo sync_repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found, "not_found", "Project not found.");
          return true;
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {
            {"project_id", project_id},
            {"sync", project_sync_to_json(sync_repo.get(project_id))},
        };
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (subpath == "/encryption-check" && req.method() == http::verb::get) {
      try {
        holder::store::ProjectRepo repo(db);
        holder::store::ProjectSyncRepo sync_repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found,
                                        "not_found",
                                        "Project not found.");
          return true;
        }
        const auto& project = project_opt.value();

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"]["project_id"] = project_id;
        payload["data"]["privacy_mode"] = project.privacy_mode;

        if (project.privacy_mode != "encrypted_git") {
          payload["data"]["check"] = {
              {"ok", true},
              {"checked_files", 0},
              {"unsafe_files", 0},
              {"unsafe_paths", nlohmann::json::array()},
              {"message", "Project is plain mode; privacy check not required."},
          };
          res = support::json_response(http::status::ok, payload);
          return true;
        }

        const auto check = holder::privacy::run_encryption_safety_check(project.root_path);
        payload["data"]["check"] = {
            {"ok", check.ok},
            {"checked_files", check.checked_files},
            {"unsafe_files", check.unsafe_paths.size()},
            {"unsafe_paths", check.unsafe_paths},
            {"message", check.message},
        };
        res = support::json_response(http::status::ok, payload);
      } catch (const holder::privacy::PrivacyError& ex) {
        res = privacy_error_response(ex);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request,
                                      "bad_request",
                                      ex.what());
      }
    } else if (subpath == "/recovery-token/export" && req.method() == http::verb::post) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("pin") || body.at("pin").is_null()) {
          res = support::error_response(http::status::bad_request,
                                        "bad_request",
                                        "Missing required fields.");
          return true;
        }
        holder::store::ProjectRepo repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found,
                                        "not_found",
                                        "Project not found.");
          return true;
        }
        if (!project_opt->project_key_id.has_value()) {
          res = support::error_response(http::status::bad_request,
                                        "bad_request",
                                        "Project has no key material configured.");
          return true;
        }
        const std::string token = holder::privacy::export_recovery_token(
            project_id,
            project_opt->project_key_id.value(),
            body.at("pin").get<std::string>(),
            project_opt->name,
            project_opt->git_remote_url);

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {
            {"project_id", project_id},
            {"key_id", project_opt->project_key_id.value()},
            {"recovery_token", token},
        };
        res = support::json_response(http::status::ok, payload);
      } catch (const holder::privacy::PrivacyError& ex) {
        res = privacy_error_response(ex);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request,
                                      "bad_request",
                                      ex.what());
      }
    } else if (subpath == "/recovery-token/import" && req.method() == http::verb::post) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("pin") || body.at("pin").is_null() ||
            !body.contains("recovery_token") || body.at("recovery_token").is_null()) {
          res = support::error_response(http::status::bad_request,
                                        "bad_request",
                                        "Missing required fields.");
          return true;
        }
        holder::store::ProjectRepo repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found,
                                        "not_found",
                                        "Project not found.");
          return true;
        }

        holder::privacy::import_recovery_token(
            repo,
            project_id,
            body.at("pin").get<std::string>(),
            body.at("recovery_token").get<std::string>(),
            support::now_epoch_seconds());

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"project_id", project_id}};
        res = support::json_response(http::status::ok, payload);
      } catch (const holder::privacy::PrivacyError& ex) {
        res = privacy_error_response(ex);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request,
                                      "bad_request",
                                      ex.what());
      }
    } else if (subpath.empty() && req.method() == http::verb::get) {
      try {
        holder::store::ProjectRepo repo(db);
        holder::store::ProjectSyncRepo sync_repo(db);
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
          data["privacy_mode"] = project.privacy_mode;
          data["project_key_id"] = project.project_key_id.has_value()
                                       ? nlohmann::json(project.project_key_id.value())
                                       : nlohmann::json(nullptr);
          data["created_at"] = project.created_at;
          data["updated_at"] = project.updated_at;
          data["sync"] = project_sync_to_json(sync_repo.get(project.project_id));

          nlohmann::json payload;
          payload["ok"] = true;
          payload["data"] = data;
          res = support::json_response(http::status::ok, payload);
        }
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::internal_server_error, "error", ex.what());
      }
    } else if (subpath.empty() && req.method() == http::verb::patch) {
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
          const bool has_privacy_mode = body.contains("privacy_mode") && !body.at("privacy_mode").is_null();
          const bool has_project_key_id = body.contains("project_key_id");
          if (!has_name && !has_root && !has_git_remote && !has_git_provider && !has_privacy_mode &&
              !has_project_key_id) {
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
              if (has_privacy_mode) {
                const auto mode = body.at("privacy_mode").get<std::string>();
                if (!is_valid_privacy_mode(mode)) {
                  res = support::error_response(http::status::bad_request,
                                                "bad_request",
                                                "privacy_mode must be encrypted_git or plain.");
                  return true;
                }
                repo.update_privacy_mode(project_id, mode, updated_at);
              }
              if (has_project_key_id) {
                if (body.at("project_key_id").is_null()) {
                  repo.update_project_key_id(project_id, std::nullopt, updated_at);
                } else {
                  repo.update_project_key_id(project_id,
                                             std::optional<std::string>(
                                                 body.at("project_key_id").get<std::string>()),
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
              const std::string effective_privacy_mode = has_privacy_mode
                                                             ? body.at("privacy_mode").get<std::string>()
                                                             : project_opt->privacy_mode;
              const std::optional<std::string> effective_project_key_id =
                  has_project_key_id
                      ? (body.at("project_key_id").is_null()
                             ? std::optional<std::string>{}
                             : std::optional<std::string>(body.at("project_key_id").get<std::string>()))
                      : project_opt->project_key_id;
              if (effective_privacy_mode == "encrypted_git") {
                const std::string repo_root =
                    has_root ? body.at("root_path").get<std::string>() : project_opt->root_path;
                auto& git = resolve_git(git_ops);
                holder::privacy::ensure_encrypted_project_ready(
                    git,
                    repo,
                    project_id,
                    repo_root,
                    effective_project_key_id,
                    updated_at,
                    uuid_v4);
              }
              nlohmann::json payload;
              payload["ok"] = true;
              payload["data"] = {{"project_id", project_id}};
              res = support::json_response(http::status::ok, payload);
            }
          }
        }
      } catch (const holder::privacy::PrivacyError& ex) {
        res = privacy_error_response(ex);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (subpath.empty() && req.method() == http::verb::delete_) {
      try {
        holder::store::ProjectRepo repo(db);
        holder::store::ProjectSyncRepo sync_repo(db);
        const auto project_opt = repo.get(project_id);
        if (!project_opt.has_value()) {
          res = support::error_response(http::status::not_found, "not_found", "Project not found.");
        } else {
          repo.remove(project_id);
          sync_repo.remove(project_id);
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
