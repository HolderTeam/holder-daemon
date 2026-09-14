#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace holder::cli {
namespace {

enum class HistoryAction {
  List,
  Show,
  Diff,
  Restore,
};

struct HistoryOptions {
  HistoryAction action = HistoryAction::List;
  bool json_output = false;
  bool help = false;
  std::optional<std::string> card_reference;
  std::vector<std::string> revisions;
  std::optional<std::string> limit;
  std::optional<std::string> cursor;
  std::optional<std::string> kind;
};

std::string history_usage() {
  return "Usage:\n"
         "  holderctl history [CARD] [--limit N] [--cursor OID] [--kind KIND] [--json]\n"
         "  holderctl history show CARD REVISION [--json]\n"
         "  holderctl history diff CARD REVISION [REVISION] [--json]\n"
         "  holderctl history restore CARD REVISION [--json]";
}

HistoryOptions parse_history_options(int argc, char* argv[]) {
  HistoryOptions options;
  std::vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--limit" || arg == "--cursor" || arg == "--kind") {
      if (i + 1 >= argc) throw std::runtime_error(history_usage());
      const std::string value = argv[++i];
      if (value.empty() || value.rfind("--", 0) == 0) {
        throw std::runtime_error(history_usage());
      }
      if (arg == "--limit")
        options.limit = value;
      else if (arg == "--cursor")
        options.cursor = value;
      else
        options.kind = value;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown history option: " + arg);
    } else {
      positional.push_back(arg);
    }
  }

  if (options.help) return options;
  if (!positional.empty() &&
      (positional.front() == "show" || positional.front() == "diff" ||
       positional.front() == "restore")) {
    const auto& action = positional.front();
    options.action = action == "show"      ? HistoryAction::Show
                     : action == "diff"    ? HistoryAction::Diff
                                            : HistoryAction::Restore;
    const bool valid_count =
        (options.action == HistoryAction::Diff &&
         (positional.size() == 3 || positional.size() == 4)) ||
        (options.action != HistoryAction::Diff && positional.size() == 3);
    if (!valid_count || options.limit.has_value() || options.cursor.has_value() ||
        options.kind.has_value()) {
      throw std::runtime_error(history_usage());
    }
    options.card_reference = positional.at(1);
    options.revisions.assign(positional.begin() + 2, positional.end());
    return options;
  }

  if (positional.size() > 1) throw std::runtime_error(history_usage());
  if (!positional.empty()) options.card_reference = positional.front();
  if (options.card_reference.has_value() && options.kind.has_value()) {
    throw std::runtime_error("--kind is available only for project history.");
  }
  return options;
}

void append_query(std::string& target, const std::string& name, const std::string& value) {
  target += target.find('?') == std::string::npos ? '?' : '&';
  target += name + "=" + url_encode_component(value);
}

nlohmann::json history_api_request(
    const holder::core::Paths& paths,
    boost::beast::http::verb method,
    const std::string& target
) {
  const auto connection = read_secure_daemon_connection(paths);
  const auto response = method == boost::beast::http::verb::get
                            ? http_json_request(
                                  connection,
                                  method,
                                  target,
                                  std::chrono::seconds(10) // LCOV_EXCL_LINE
                              )
                            : http_json_request(
                                  connection,
                                  method,
                                  target,
                                  std::chrono::seconds(30), // LCOV_EXCL_LINE
                                  nlohmann::json::object()
                              );
  if (response.status == boost::beast::http::status::ok && response.payload.value("ok", false)) {
    return response.payload;
  }

  const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
  const auto message = api_error_message(response, fallback);
  std::string code = "history_request_failed";
  nlohmann::json details = nlohmann::json::object();
  if (response.payload.contains("error") && response.payload.at("error").is_object()) {
    const auto& error = response.payload.at("error");
    code = json_string(error, "code", code);
    if (error.contains("details")) details = error.at("details");
  }
  throw CliError(code, message, std::move(details), message);
}

std::string short_revision(const std::string& oid) {
  return oid.substr(0, std::min<std::size_t>(8, oid.size()));
}

std::string table_text(std::string value) {
  std::replace(value.begin(), value.end(), '\t', ' ');
  const auto newline = value.find_first_of("\r\n");
  if (newline != std::string::npos) value.erase(newline);
  return value;
}

void print_continuation(const nlohmann::json& data) {
  if (data.contains("next_cursor") && !data.at("next_cursor").is_null()) {
    std::cout << "Next cursor: " << data.at("next_cursor").get<std::string>() << "\n";
  }
  if (data.value("scan_limited", false)) {
    std::cout << "History scan limit reached; use the next cursor to continue.\n";
  }
}

void print_project_history(const nlohmann::json& data) {
  const auto& activities = data.at("activities");
  if (activities.empty()) {
    std::cout << "No project history.\n";
    print_continuation(data);
    return;
  }

  std::cout << "REVISION\tAUTHOR\tCOMMITTED\tKINDS\tMESSAGE\n";
  for (const auto& activity : activities) {
    std::vector<std::string> kinds;
    for (const auto& object : activity.at("affected_objects")) {
      const auto kind = json_string(object, "kind");
      if (std::find(kinds.begin(), kinds.end(), kind) == kinds.end()) kinds.push_back(kind);
    }
    std::ostringstream kind_text;
    for (std::size_t i = 0; i < kinds.size(); ++i) {
      if (i != 0) kind_text << ',';
      kind_text << kinds.at(i);
    }
    std::cout << short_revision(json_string(activity, "oid")) << "\t"
              << table_text(json_string(activity.at("author"), "name")) << "\t"
              << activity.value("committed_at", 0LL) << "\t" << kind_text.str() << "\t"
              << table_text(json_string(activity, "message")) << "\n";
  }
  print_continuation(data);
}

void print_card_history(const nlohmann::json& data) {
  const auto& entries = data.at("entries");
  if (entries.empty()) {
    std::cout << "No card history.\n";
    print_continuation(data);
    return;
  }

  std::cout << "REVISION\tSAVES\tKIND\tUPDATED\tSUMMARY\n";
  for (const auto& entry : entries) {
    std::cout << short_revision(json_string(entry, "last_oid")) << "\t"
              << entry.value("commit_count", 0) << "\t" << json_string(entry, "kind") << "\t"
              << entry.value("ended_at", 0LL) << "\t"
              << table_text(json_string(entry, "summary")) << "\n";
  }
  print_continuation(data);
}

void print_snapshot(const nlohmann::json& data) {
  const auto& snapshot = data.at("snapshot");
  const auto oid = json_string(snapshot, "oid");
  if (!snapshot.value("exists", false)) {
    std::cout << "Card did not exist at revision " << oid << ".\n";
    return;
  }

  std::cout << "Revision: " << oid << "\nTitle: " << json_string(snapshot, "title") << "\n\n";
  const auto body = json_string(snapshot, "body");
  std::cout << body;
  if (body.empty() || body.back() != '\n') std::cout << '\n';
}

void print_comparison(const nlohmann::json& data) {
  const auto& from = data.at("from");
  const auto& to = data.at("to");
  std::cout << json_string(data, "summary") << "\nFrom: ";
  if (from.value("exists", false))
    std::cout << json_string(from, "oid");
  else
    std::cout << "(card did not exist)";
  std::cout << "\nTo: " << json_string(to, "oid") << "\n\n";
  for (const auto& line : data.at("lines")) {
    std::cout << json_string(line, "origin", " ") << json_string(line, "text") << "\n";
  }
  if (data.value("truncated", false)) std::cout << "[diff truncated]\n";
}

void print_restore(const nlohmann::json& data) {
  std::cout << "Restored card " << display_card_id(json_string(data, "card_id")) << ": "
            << json_string(data, "title") << "\nSource revision: "
            << json_string(data, "restored_from_oid") << "\nResult revision: "
            << json_string(data, "result_oid") << "\nState: "
            << (data.contains("deleted_at") && !data.at("deleted_at").is_null() ? "trashed"
                                                                                : "live")
            << "\n";
}

} // namespace

int command_history(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_history_options(argc, argv);
  if (options.help) {
    std::cout << history_usage() << "\n";
    return 0;
  }

  try {
    const auto project = require_current_project_payload(paths);
    const auto project_id = json_string(project, "project_id");
    std::optional<std::string> card_id;
    if (options.card_reference.has_value()) {
      card_id = resolve_card_reference(
          paths,
          project_id,
          *options.card_reference,
          CardReferenceScope::Either
      );
    }

    std::string target = "/projects/" + url_encode_component(project_id) + "/history";
    if (card_id.has_value()) target += "/cards/" + url_encode_component(*card_id);

    boost::beast::http::verb method = boost::beast::http::verb::get;
    if (options.action == HistoryAction::List) {
      if (options.limit.has_value()) append_query(target, "limit", *options.limit);
      if (options.cursor.has_value()) append_query(target, "cursor", *options.cursor);
      if (options.kind.has_value()) append_query(target, "kind", *options.kind);
    } else if (options.action == HistoryAction::Show) {
      target += "/snapshot";
      append_query(target, "oid", options.revisions.front());
    } else if (options.action == HistoryAction::Diff) {
      target += "/compare";
      if (options.revisions.size() == 1) {
        append_query(target, "mode", "change");
        append_query(target, "to", options.revisions.front());
      } else {
        append_query(target, "mode", "since");
        append_query(target, "from", options.revisions.at(0));
        append_query(target, "to", options.revisions.at(1));
      }
    } else {
      target += "/restore";
      append_query(target, "oid", options.revisions.front());
      method = boost::beast::http::verb::post;
    }

    const auto payload = history_api_request(paths, method, target);
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
      return 0;
    }

    const auto& data = payload.at("data");
    if (options.action == HistoryAction::Show)
      print_snapshot(data);
    else if (options.action == HistoryAction::Diff)
      print_comparison(data);
    else if (options.action == HistoryAction::Restore)
      print_restore(data);
    else if (card_id.has_value())
      print_card_history(data);
    else
      print_project_history(data);
    return 0;
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to inspect history: ") + ex.what());
  }
}

} // namespace holder::cli
