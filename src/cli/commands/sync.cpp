#include "cli/commands/Commands.h"
#include "cli/commands/Support.h"

#include <boost/system/system_error.hpp>

#include <iostream>
#include <optional>
#include <string>

namespace holder::cli {
namespace {

const char* sync_usage() {
  return "Usage:\n"
         "  holderctl sync status [--project <id-or-name>] [--json]\n"
         "  holderctl sync remote [URL] [--project <id-or-name>] [--json]\n"
         "  holderctl sync disconnect [--project <id-or-name>] [--json]\n"
         "\nInspect recorded Git sync state and configure the project's remote.\n"
         "Remote without a URL shows the configured remote; disconnect removes it.\n"
         "Use --project to choose an exact project ID or name without changing the selection.\n"
         "Omit --project to use the selected project (or Home by default).\n"
         "Counts and results describe the daemon's last observation, not a fresh Git check.\n"
         "Live activity and remote-behind counts are unavailable.\n"
         "Error text containing URLs or credential markers is redacted in all output.\n"
         "Remote URLs containing credentials, query strings, or fragments are redacted.\n"
         "\nExamples:\n"
         "  holderctl sync status\n"
         "  holderctl sync status --json\n"
         "  holderctl sync status --project \"Work\"\n"
         "  holderctl sync status --project \"Work\" --json\n"
         "  holderctl sync remote --project \"Work\"\n"
         "  holderctl sync remote git@example.com:team/work.git --project \"Work\"\n"
         "  holderctl sync disconnect --project \"Work\" --json";
}

// Git diagnostics may embed arbitrary credentials. Withhold the entire diagnostic
// when it contains a URL or credential marker rather than guessing secret boundaries.
std::string safe_diagnostic(const std::string& text) {
  const auto lower = lower_ascii(text);
  for (const auto* marker :
       {"://", "@", "token", "password", "credential", "bearer", "authorization"}) {
    if (lower.find(marker) != std::string::npos) return "[redacted]";
  }
  return text;
}

std::string safe_remote_url(const std::string& url) {
  if (url.find_first_of("?#") != std::string::npos) return "[redacted]";
  for (const char value : url) {
    const auto ch = static_cast<unsigned char>(value);
    if (ch < 32 || ch == 127) return "[redacted]";
  }
  const auto scheme = url.find("://");
  if (scheme != std::string::npos) {
    const auto end = url.find('/', scheme + 3);
    const auto authority =
        url.substr(scheme + 3, end == std::string::npos ? end : end - (scheme + 3));
    if (authority.find('@') != std::string::npos) return "[redacted]";
  } else if (url.find('@') != std::string::npos &&
             (url.rfind("git@", 0) != 0 || url.rfind('@') != 3)) {
    return "[redacted]";
  }
  return url;
}

void redact_diagnostics(nlohmann::json& value) {
  if (value.is_string()) {
    value = safe_diagnostic(value.get<std::string>());
  } else if (value.is_structured()) {
    for (auto& child : value)
      redact_diagnostics(child);
  }
}

std::string sync_project_id(
    const holder::core::Paths& paths,
    const std::optional<std::string>& reference
) {
  if (reference) {
    const auto projects = card_api_request(paths, boost::beast::http::verb::get, "/projects");
    return json_string(resolve_project(projects.at("data"), *reference), "project_id");
  }
  if (const auto id = read_configured_project_id(paths)) return *id;
  const auto projects = card_api_request(paths, boost::beast::http::verb::get, "/projects");
  for (const auto& project : projects.at("data")) {
    if (is_home_project(project)) return json_string(project, "project_id");
  }
  throw CliError("not_found", "Default Home project not found.");
}

void print_sync_status(const nlohmann::json& data, bool has_remote) {
  const auto& sync = data.at("sync");
  std::cout << "Project: " << json_string(data, "project_id") << "\n";
  std::cout << "Remote: " << (has_remote ? "configured" : "none") << "\n";
  const auto changes = sync.at("uncommitted_changes_count").get<int>();
  const auto commits = sync.at("unpushed_commits_count").get<int>();
  std::cout << "Recorded state: ";
  if (sync.at("updated_at").is_null()) {
    std::cout << "no sync state recorded";
  } else if (changes == 0 && commits == 0) {
    std::cout << "clean";
  } else {
    if (changes != 0) std::cout << changes << " local changes";
    if (changes != 0 && commits != 0) std::cout << ", ";
    if (commits != 0) std::cout << commits << " unpushed commits";
  }
  std::cout << "\nLast pull: " << json_string(sync, "last_pull_status", "not recorded")
            << "\nLast push: " << json_string(sync, "last_push_status", "not recorded") << "\n";
  if (!sync.at("last_sync_error").is_null()) {
    std::cout << "Last failure: " << json_string(sync, "last_sync_error") << "\n";
  }
  for (const auto* key :
       {"updated_at", "last_sync_error_at", "next_retry_at", "next_pull_retry_at"}) {
    if (!sync.at(key).is_null()) std::cout << key << ": " << sync.at(key) << "\n";
  }
}

} // namespace

int command_sync(const holder::core::Paths& paths, int argc, char* argv[]) {
  bool json_output = false;
  bool help = false;
  std::string action;
  std::optional<std::string> project_reference;
  std::optional<std::string> remote_url;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json")
      json_output = true;
    else if (arg == "--help" || arg == "-h")
      help = true;
    else if (arg == "--project") {
      if (project_reference || i + 1 >= argc) {
        throw CliError("bad_request", sync_usage(), nlohmann::json::object(), "", 2);
      }
      const std::string value = argv[++i];
      if (trim_ascii_whitespace(value).empty() || value.front() == '-') {
        throw CliError("bad_request", sync_usage(), nlohmann::json::object(), "", 2);
      }
      project_reference = value;
    } else if (action.empty() && (arg == "status" || arg == "remote" || arg == "disconnect")) {
      action = arg;
    } else if (action == "remote" && !remote_url && !trim_ascii_whitespace(arg).empty() &&
               arg.front() != '-') {
      remote_url = arg;
    } else {
      throw CliError("bad_request", sync_usage(), nlohmann::json::object(), "", 2);
    }
  }
  if (help) {
    std::cout << sync_usage() << "\n";
    return 0;
  }
  if (action.empty()) throw CliError("bad_request", sync_usage(), nlohmann::json::object(), "", 2);

  try {
    const auto project_id = sync_project_id(paths, project_reference);
    const auto target = "/projects/" + url_encode_component(project_id);
    if (action != "status") {
      const bool mutation = action == "disconnect" || remote_url.has_value();
      auto body = nlohmann::json::object();
      if (mutation) {
        body["git_remote_url"] = remote_url ? nlohmann::json(*remote_url) : nlohmann::json(nullptr);
        body["updated_at"] = now_epoch_seconds();
      }
      auto payload = card_api_request(
          paths,
          mutation ? boost::beast::http::verb::patch : boost::beast::http::verb::get,
          target,
          body
      );
      if (!mutation) {
        auto& data = payload.at("data");
        if (!data.at("git_remote_url").is_null()) {
          data["git_remote_url"] = safe_remote_url(json_string(data, "git_remote_url"));
        }
        redact_diagnostics(data.at("sync").at("last_sync_error"));
      }
      if (json_output) {
        std::cout << payload.dump(2) << "\n";
      } else if (mutation) {
        const bool changed = payload.at("data").at("git_remote_changed").get<bool>();
        std::cout << "Project: " << json_string(payload.at("data"), "project_id") << "\n";
        if (action == "disconnect") {
          std::cout << (changed ? "Disconnected.\n" : "Already disconnected.\n");
        } else {
          std::cout << (changed ? "Remote set: " : "Remote unchanged: ")
                    << safe_remote_url(*remote_url) << "\n";
        }
      } else {
        const auto remote = json_string(payload.at("data"), "git_remote_url");
        std::cout << "Remote: " << (remote.empty() ? "none" : remote) << "\n";
      }
      return 0;
    }
    auto payload =
        card_api_request(paths, boost::beast::http::verb::get, target + "/git/sync-status");
    redact_diagnostics(payload.at("data").at("sync").at("last_sync_error"));
    if (json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      const auto project = card_api_request(paths, boost::beast::http::verb::get, target);
      const auto remote = json_string(project.at("data"), "git_remote_url");
      print_sync_status(payload.at("data"), !remote.empty());
    }
    return 0;
  } catch (const CliError& ex) {
    auto details = ex.details();
    redact_diagnostics(details);
    throw CliError(ex.code(), safe_diagnostic(ex.message()), details, "", ex.exit_code());
  } catch (const boost::system::system_error& ex) {
    throw CliError("network_error", safe_diagnostic(ex.what()));
  } catch (const std::exception& ex) {
    throw CliError(
        action == "status" ? "sync_status_failed" : "sync_request_failed",
        safe_diagnostic(ex.what())
    );
  }
}

} // namespace holder::cli
