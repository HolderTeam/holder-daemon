#include "cli/commands/Commands.h"
#include "cli/commands/Support.h"

#include <boost/system/system_error.hpp>

#include <iostream>
#include <string>

namespace holder::cli {
namespace {

const char* sync_usage() {
  return "Usage: holderctl sync status [--json]\n"
         "\nInspect the current project's recorded Git sync state.\n"
         "Counts and results describe the daemon's last observation, not a fresh Git check.\n"
         "Live activity and remote-behind counts are unavailable.\n"
         "Error text containing URLs or credential markers is redacted in all output.\n"
         "\nExamples:\n"
         "  holderctl sync status\n"
         "  holderctl sync status --json";
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

void redact_diagnostics(nlohmann::json& value) {
  if (value.is_string()) {
    value = safe_diagnostic(value.get<std::string>());
  } else if (value.is_structured()) {
    for (auto& child : value)
      redact_diagnostics(child);
  }
}

std::string current_sync_project_id(const holder::core::Paths& paths) {
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
  bool status = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json")
      json_output = true;
    else if (arg == "--help" || arg == "-h")
      help = true;
    else if (arg == "status" && !status)
      status = true;
    else
      throw CliError("bad_request", sync_usage(), nlohmann::json::object(), "", 2);
  }
  if (help) {
    std::cout << sync_usage() << "\n";
    return 0;
  }
  if (!status) throw CliError("bad_request", sync_usage(), nlohmann::json::object(), "", 2);

  try {
    const auto project_id = current_sync_project_id(paths);
    const auto target = "/projects/" + url_encode_component(project_id);
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
    throw CliError("sync_status_failed", safe_diagnostic(ex.what()));
  }
}

} // namespace holder::cli
