#include "api/routes/GoogleDriveOAuthRoutes.h"

#include "api/support/HttpQuery.h"
#include "api/support/HttpResponses.h"
#include "api/support/Time.h"
#include "resource/LocationBindingStore.h"
#include "resource/LocationRepo.h"
#include "resource/LocationStore.h"
#include "storage/google/DriveApi.h"
#include "storage/google/GoogleOAuth.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

// Google's redirect_uri is required to match exactly what the authorize request
// registered, so both sides build it identically from the incoming request's own Host
// header -- since the desktop app and the browser both reach this daemon on the same
// 127.0.0.1 address and port, whatever the desktop app used to reach /authorize is
// exactly what Google needs to redirect back to. Falls back to a literal loopback
// address only if a request somehow arrives with no Host header at all.
std::string oauth_redirect_uri(
    const http::request<http::string_body>& req,
    const std::string& location_id
) {
  std::string host = "127.0.0.1";
  if (req.count(http::field::host) > 0) {
    host = std::string(req[http::field::host]);
  }
  return "http://" + host + "/locations/" + location_id + "/oauth/google-drive/callback";
}

std::string percent_decode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '%' && i + 2 < value.size() &&
        std::isxdigit(static_cast<unsigned char>(value[i + 1])) != 0 &&
        std::isxdigit(static_cast<unsigned char>(value[i + 2])) != 0) {
      const auto hex = value.substr(i + 1, 2);
      out.push_back(static_cast<char>(std::strtol(hex.c_str(), nullptr, 16)));
      i += 2;
    } else if (value[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

struct PendingOAuthAttempt {
  std::string code_verifier;
  std::string state;
  std::string project_id;
  long long created_at = 0;
};

// A pending attempt lives only between "the user clicked Connect" and "Google redirected
// back" -- in-memory and process-lifetime is enough, the same shape as
// AiResourceRoutes.cpp's own import_jobs map. One-shot: the callback erases an entry the
// moment it reads it, since an authorization code is itself single-use anyway.
std::mutex pending_oauth_mutex;
std::unordered_map<std::string, PendingOAuthAttempt> pending_oauth_attempts;

// Generous enough for "open a browser tab and finish signing in", short enough that an
// abandoned attempt's state/code_verifier can't be replayed much later.
constexpr long long kPendingOAuthTtlSeconds = 600;

constexpr const char* kDriveFileScope = "https://www.googleapis.com/auth/drive.file";

// /locations/{id}/oauth/google-drive/callback -- the one path shape this file parses
// itself, since the callback is dispatched before the normal /locations/{id}/<action>
// routing (in handle_ai_resource_routes) ever runs.
std::optional<std::string> parse_callback_location_id(const std::string& path) {
  static const std::string kPrefix = "/locations/";
  static const std::string kSuffix = "/oauth/google-drive/callback";
  if (path.rfind(kPrefix, 0) != 0) return std::nullopt;
  if (path.size() <= kPrefix.size() + kSuffix.size()) return std::nullopt;
  if (path.compare(path.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
    return std::nullopt;
  }
  return path.substr(kPrefix.size(), path.size() - kPrefix.size() - kSuffix.size());
}

http::response<http::string_body> html_response(http::status status, const std::string& title, const std::string& message) {
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, "text/html; charset=utf-8");
  res.keep_alive(false);
  res.body() = "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + title +
               "</title></head><body style=\"font-family: sans-serif; max-width: 32rem; "
               "margin: 3rem auto; padding: 0 1rem;\"><h1>" +
               title + "</h1><p>" + message + "</p></body></html>";
  res.prepare_payload();
  return res;
}

} // namespace

bool handle_google_drive_oauth_authorize_route(
    const std::string& location_id,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db
) {
  try {
    const auto location = holder::resource::LocationRepo(db).get(location_id);
    if (!location.has_value()) {
      throw std::runtime_error("location not found");
    }
    if (location->provider != "google-drive") {
      throw std::invalid_argument("location is not a google-drive location");
    }

    const auto client = holder::storage::google::google_oauth_client_from_env();
    const auto pkce = holder::storage::google::generate_pkce_challenge();
    const auto state = holder::storage::google::generate_state();
    const auto redirect_uri = oauth_redirect_uri(req, location_id);

    {
      std::lock_guard<std::mutex> lock(pending_oauth_mutex);
      pending_oauth_attempts[location_id] = PendingOAuthAttempt{
          pkce.code_verifier, state, location->project_id, support::now_epoch_seconds()
      };
    }

    const auto authorization_url = holder::storage::google::build_authorization_url(
        client, redirect_uri, kDriveFileScope, pkce.code_challenge, state
    );

    res = support::json_response(
        http::status::ok,
        {{"ok", true}, {"data", {{"authorization_url", authorization_url}}}}
    );
  } catch (const std::exception& ex) {
    res = support::error_response(http::status::bad_request, "bad_request", ex.what());
  }
  return true;
}

bool handle_google_drive_oauth_callback_route(
    const std::string& path,
    const std::string& query_string,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::privacy::SecretStore* secret_store,
    holder::git::GitOps* git_ops
) {
  const auto location_id = parse_callback_location_id(path);
  if (!location_id.has_value() || req.method() != http::verb::get) {
    return false;
  }

  auto param = [&](const std::string& key) {
    return percent_decode(support::query_param_value(query_string, key));
  };
  const auto code = param("code");
  const auto state = param("state");
  const auto oauth_error = param("error");

  PendingOAuthAttempt attempt;
  {
    std::lock_guard<std::mutex> lock(pending_oauth_mutex);
    const auto found = pending_oauth_attempts.find(*location_id);
    if (found == pending_oauth_attempts.end()) {
      res = html_response(
          http::status::bad_request,
          "Connection expired",
          "This Google Drive connection attempt is no longer valid. Return to Holder and "
          "try connecting again."
      );
      return true;
    }
    attempt = found->second;
    pending_oauth_attempts.erase(found);
  }

  if (support::now_epoch_seconds() - attempt.created_at > kPendingOAuthTtlSeconds) {
    res = html_response(
        http::status::bad_request,
        "Connection expired",
        "This Google Drive connection attempt took too long. Return to Holder and try "
        "connecting again."
    );
    return true;
  }
  if (state.empty() || state != attempt.state) {
    res = html_response(
        http::status::bad_request,
        "Connection failed",
        "This request could not be verified. Return to Holder and try connecting again."
    );
    return true;
  }
  if (!oauth_error.empty()) {
    res = html_response(
        http::status::ok,
        "Connection cancelled",
        "Google Drive was not connected. You can close this window and return to Holder."
    );
    return true;
  }
  if (code.empty()) {
    res = html_response(
        http::status::bad_request,
        "Connection failed",
        "Google did not return an authorization code. Return to Holder and try "
        "connecting again."
    );
    return true;
  }

  try {
    if (secret_store == nullptr) {
      throw std::runtime_error("secret store unavailable");
    }
    const auto client = holder::storage::google::google_oauth_client_from_env();
    const auto redirect_uri = oauth_redirect_uri(req, *location_id);
    const auto token = holder::storage::google::exchange_authorization_code(
        client, redirect_uri, code, attempt.code_verifier
    );
    if (token.refresh_token.empty()) {
      // Google only issues a refresh_token when the user actually sees and grants the
      // consent screen (access_type=offline + prompt=consent in the authorize URL should
      // guarantee this, but Google's behavior here has changed before -- see
      // GoogleOAuth.cpp's own comment on why prompt=consent is always forced).
      throw std::runtime_error(
          "Google did not return a refresh token -- disconnect Holder's access in your "
          "Google Account settings and try connecting again"
      );
    }

    const auto folder_id =
        holder::storage::google::find_or_create_holder_resources_folder(token.access_token);

    auto location = holder::resource::LocationRepo(db).get(*location_id);
    if (!location.has_value()) {
      throw std::runtime_error("location no longer exists");
    }
    location->configuration["folder_id"] = folder_id;
    location->updated_at = support::now_epoch_seconds();
    if (git_ops != nullptr) {
      holder::resource::LocationStore(db, nullptr, git_ops).put(*location);
    } else {
      holder::resource::LocationRepo(db).put(*location);
    }

    holder::resource::LocationBinding binding;
    binding.provider = "google-drive";
    binding.values = {{"refresh_token", token.refresh_token}};
    holder::resource::LocationBindingStore bindings(*secret_store);
    bindings.bind(
        attempt.project_id, *location_id, binding, "Connected", support::now_epoch_seconds()
    );

    res = html_response(
        http::status::ok,
        "Google Drive connected",
        "You can close this window and return to Holder."
    );
  } catch (const std::exception& ex) {
    res = html_response(
        http::status::internal_server_error,
        "Connection failed",
        std::string("Something went wrong: ") + ex.what() +
            ". Return to Holder and try connecting again."
    );
  }
  return true;
}

} // namespace holder::api::routes
