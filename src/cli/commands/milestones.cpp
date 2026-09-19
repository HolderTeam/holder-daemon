#include "cli/commands/Commands.h"

#include "cli/commands/MilestoneDateTime.h"
#include "cli/commands/Support.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace holder::cli {
namespace {

struct MilestonesOptions {
  bool json_output = false;
  bool help = false;
  std::string card_reference;
};

enum class MilestoneAction {
  Add,
  Edit,
  Remove,
};

struct MilestoneOptions {
  MilestoneAction action = MilestoneAction::Add;
  bool json_output = false;
  bool help = false;
  bool all_day = false;
  bool timed = false;
  bool clear_end = false;
  bool clear_kind = false;
  bool clear_description = false;
  std::string card_reference;
  std::string value;
  std::optional<std::string> start;
  std::optional<std::string> end;
  std::optional<std::string> kind;
  std::optional<std::string> description;
};

struct CalendarOptions {
  bool json_output = false;
  bool help = false;
  std::optional<std::string> from;
  std::optional<std::string> to;
};

std::string milestones_usage() { return "Usage: holderctl milestones CARD [--json]"; }

std::string milestone_usage() {
  return "Usage:\n"
         "  holderctl milestone add CARD WHEN [--end WHEN] [--kind KIND] "
         "[--description TEXT] [--all-day] [--json]\n"
         "  holderctl milestone edit CARD MILESTONE_ID [--start WHEN] [--end WHEN | "
         "--clear-end] [--kind KIND | --clear-kind] [--description TEXT | "
         "--clear-description] [--all-day | --timed] [--json]\n"
         "  holderctl milestone remove CARD MILESTONE_ID [--json]\n"
         "\n"
         "WHEN is YYYY-MM-DD for an all-day local date, or an RFC 3339 timestamp with an "
         "explicit offset for a timed milestone. --all-day is accepted only with date values.";
}

std::string calendar_usage() {
  return "Usage: holderctl calendar [--from WHEN] [--to WHEN] [--json]\n"
         "Date-only bounds cover inclusive local calendar days. Timed bounds require RFC 3339 "
         "with an explicit offset. Missing bounds use the local day containing the other bound; "
         "with neither bound, calendar shows today.";
}

ParsedMilestoneWhen parse_cli_milestone_when(const std::string& value) {
  try {
    return parse_milestone_when(value);
  } catch (const std::invalid_argument& ex) {
    throw CliError(
        "invalid_milestone_time",
        "Invalid milestone date/time.",
        {{"value", value}, {"reason", ex.what()}},
        std::string("Invalid milestone date/time: ") + ex.what()
    );
  }
}

CalendarRange resolve_cli_calendar_range(
    const std::optional<std::string>& from,
    const std::optional<std::string>& to
) {
  try {
    return resolve_calendar_range(from, to, now_epoch_seconds());
  } catch (const std::invalid_argument& ex) {
    nlohmann::json details{{"reason", ex.what()}};
    if (from.has_value()) details["from"] = *from;
    if (to.has_value()) details["to"] = *to;
    throw CliError(
        "invalid_calendar_range",
        "Invalid calendar range.",
        std::move(details),
        std::string("Invalid calendar range: ") + ex.what()
    );
  }
}

MilestonesOptions parse_milestones_options(int argc, char* argv[]) {
  MilestonesOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json")
      options.json_output = true;
    else if (arg == "--help" || arg == "-h")
      options.help = true;
    else if (arg.rfind("--", 0) == 0)
      throw std::runtime_error("Unknown milestones option: " + arg);
    else if (options.card_reference.empty())
      options.card_reference = arg;
    else
      throw std::runtime_error(milestones_usage());
  }
  if (!options.help && options.card_reference.empty()) {
    throw std::runtime_error(milestones_usage());
  }
  return options;
}

MilestoneOptions parse_milestone_options(int argc, char* argv[]) {
  MilestoneOptions options;
  std::vector<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--all-day") {
      options.all_day = true;
    } else if (arg == "--timed") {
      options.timed = true;
    } else if (arg == "--clear-end") {
      options.clear_end = true;
    } else if (arg == "--clear-kind") {
      options.clear_kind = true;
    } else if (arg == "--clear-description") {
      options.clear_description = true;
    } else if (arg == "--start" || arg == "--end" || arg == "--kind" || arg == "--description") {
      if (i + 1 >= argc) throw std::runtime_error(milestone_usage());
      const std::string value = argv[++i];
      if (value.rfind("--", 0) == 0) throw std::runtime_error(milestone_usage());
      if (arg == "--start")
        options.start = value;
      else if (arg == "--end")
        options.end = value;
      else if (arg == "--kind")
        options.kind = value;
      else
        options.description = value;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown milestone option: " + arg);
    } else {
      positional.push_back(arg);
    }
  }

  if (options.help) return options;
  if (positional.size() != 3 || (positional.front() != "add" && positional.front() != "edit" &&
                                 positional.front() != "remove")) {
    throw std::runtime_error(milestone_usage());
  }
  if (positional.front() == "add")
    options.action = MilestoneAction::Add;
  else if (positional.front() == "edit")
    options.action = MilestoneAction::Edit;
  else
    options.action = MilestoneAction::Remove;
  options.card_reference = positional.at(1);
  options.value = positional.at(2);
  if (options.action == MilestoneAction::Remove &&
      (options.all_day || options.timed || options.start.has_value() || options.end.has_value() ||
       options.clear_end || options.kind.has_value() || options.clear_kind ||
       options.description.has_value() || options.clear_description)) {
    throw std::runtime_error(milestone_usage());
  }
  if (options.action == MilestoneAction::Add &&
      (options.timed || options.start.has_value() || options.clear_end || options.clear_kind ||
       options.clear_description)) {
    throw std::runtime_error(milestone_usage());
  }
  if ((options.end.has_value() && options.clear_end) ||
      (options.kind.has_value() && options.clear_kind) ||
      (options.description.has_value() && options.clear_description) ||
      (options.all_day && options.timed)) {
    throw std::runtime_error(milestone_usage());
  }
  if (options.action == MilestoneAction::Edit && !options.start.has_value() &&
      !options.end.has_value() && !options.clear_end && !options.kind.has_value() &&
      !options.clear_kind && !options.description.has_value() && !options.clear_description &&
      !options.all_day && !options.timed) {
    throw std::runtime_error(milestone_usage());
  }
  return options;
}

CalendarOptions parse_calendar_options(int argc, char* argv[]) {
  CalendarOptions options;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--from" || arg == "--to") {
      if (i + 1 >= argc) throw std::runtime_error(calendar_usage());
      const std::string value = argv[++i];
      if (value.empty() || value.rfind("--", 0) == 0) {
        throw std::runtime_error(calendar_usage());
      }
      if (arg == "--from")
        options.from = value;
      else
        options.to = value;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown calendar option: " + arg);
    } else {
      throw std::runtime_error(calendar_usage());
    }
  }
  return options;
}

nlohmann::json milestone_api_request(
    const holder::core::Paths& paths,
    boost::beast::http::verb method,
    const std::string& target,
    boost::beast::http::status success,
    const std::optional<nlohmann::json>& body = std::nullopt
) {
  const auto connection = read_secure_daemon_connection(paths);
  const auto response = http_json_request(
      connection,
      method,
      target,
      method == boost::beast::http::verb::get ? std::chrono::seconds(10) : std::chrono::seconds(30),
      body
  );
  if (response.status == success && response.payload.value("ok", false)) return response.payload;

  const auto fallback = "HTTP " + std::to_string(static_cast<unsigned>(response.status));
  const auto message = api_error_message(response, fallback);
  std::string code = "milestone_request_failed";
  nlohmann::json details = nlohmann::json::object();
  if (response.payload.contains("error") && response.payload.at("error").is_object()) {
    const auto& error = response.payload.at("error");
    code = json_string(error, "code", code);
    if (error.contains("details")) details = error.at("details");
  }
  throw CliError(code, message, std::move(details), message);
}

std::string optional_text(const nlohmann::json& value, const char* key) {
  if (!value.contains(key) || value.at(key).is_null()) return "";
  return value.at(key).get<std::string>();
}

std::string milestone_range(const nlohmann::json& milestone) {
  const bool all_day = milestone.value("all_day", false);
  std::string text = format_milestone_when(milestone.at("start_at").get<long long>(), all_day);
  if (milestone.contains("end_at") && !milestone.at("end_at").is_null()) {
    text += " – " + format_milestone_when(milestone.at("end_at").get<long long>(), all_day);
  }
  return text;
}

void print_milestones(const nlohmann::json& data) {
  if (data.empty()) {
    std::cout << "No milestones.\n";
    return;
  }
  std::cout << "MILESTONE_ID\tWHEN\tKIND\tDESCRIPTION\n";
  for (const auto& milestone : data) {
    std::cout << json_string(milestone, "milestone_id") << "\t" << milestone_range(milestone)
              << "\t" << optional_text(milestone, "kind") << "\t"
              << optional_text(milestone, "description") << "\n";
  }
}

struct CalendarEvent {
  long long when = 0;
  std::optional<long long> end;
  bool all_day = false;
  std::string type;
  std::string event_id;
  std::string card_id;
  std::string title;
  std::string details;
};

void append_calendar_events(const nlohmann::json& data, std::vector<CalendarEvent>& events) {
  for (const auto& milestone : data.at("milestones")) {
    CalendarEvent event;
    event.when = milestone.at("start_at").get<long long>();
    if (!milestone.at("end_at").is_null()) {
      event.end = milestone.at("end_at").get<long long>();
    }
    event.all_day = milestone.value("all_day", false);
    event.type = "milestone";
    event.event_id = json_string(milestone, "milestone_id");
    event.card_id = json_string(milestone, "card_id");
    event.title = json_string(milestone, "card_title");
    event.details = optional_text(milestone, "kind");
    const auto description = optional_text(milestone, "description");
    if (!description.empty()) {
      if (!event.details.empty()) event.details += ": ";
      event.details += description;
    }
    events.push_back(std::move(event));
  }
  for (const auto& card : data.at("created_cards")) {
    events.push_back({
        .when = card.at("created_at").get<long long>(),
        .end = std::nullopt,
        .all_day = false,
        .type = "card-created",
        .event_id = {},
        .card_id = json_string(card, "card_id"),
        .title = json_string(card, "title"),
        .details = {},
    });
  }
  for (const auto& card : data.at("updated_cards")) {
    events.push_back({
        .when = card.at("updated_at").get<long long>(),
        .end = std::nullopt,
        .all_day = false,
        .type = "card-updated",
        .event_id = {},
        .card_id = json_string(card, "card_id"),
        .title = json_string(card, "title"),
        .details = {},
    });
  }
}

void print_calendar(const nlohmann::json& data) {
  std::vector<CalendarEvent> events;
  append_calendar_events(data, events);
  std::sort(events.begin(), events.end(), [](const auto& lhs, const auto& rhs) {
    if (lhs.when != rhs.when) return lhs.when < rhs.when;
    if (lhs.type != rhs.type) return lhs.type < rhs.type;
    return lhs.card_id < rhs.card_id;
  });
  if (events.empty()) {
    std::cout << "No calendar events.\n";
    return;
  }

  std::vector<std::string> card_ids;
  card_ids.reserve(events.size());
  for (const auto& event : events)
    card_ids.push_back(event.card_id);
  const auto displayed_ids = display_card_ids(card_ids);

  std::cout << "WHEN\tTYPE\tEVENT_ID\tCARD_ID\tTITLE\tDETAILS\n";
  for (std::size_t i = 0; i < events.size(); ++i) {
    const auto& event = events.at(i);
    auto when = format_milestone_when(event.when, event.all_day);
    if (event.end.has_value()) {
      when += " – " + format_milestone_when(*event.end, event.all_day);
    }
    std::cout << when << "\t" << event.type << "\t"
              << (event.event_id.empty() ? "-" : event.event_id) << "\t" << displayed_ids.at(i)
              << "\t" << event.title << "\t" << event.details << "\n";
  }
}

} // namespace

int command_milestones(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_milestones_options(argc, argv);
  if (options.help) {
    std::cout << milestones_usage() << "\n";
    return 0;
  }
  try {
    const auto project = require_current_project_payload(paths);
    const auto project_id = json_string(project, "project_id");
    const auto card_id =
        resolve_card_reference(paths, project_id, options.card_reference, CardReferenceScope::Live);
    const auto payload = milestone_api_request(
        paths,
        boost::beast::http::verb::get,
        "/cards/" + url_encode_component(card_id) + "/milestones",
        boost::beast::http::status::ok
    );
    if (options.json_output)
      std::cout << payload.dump(2) << "\n";
    else
      print_milestones(payload.at("data"));
    return 0;
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list milestones: ") + ex.what());
  }
}

int command_milestone(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_milestone_options(argc, argv);
  if (options.help) {
    std::cout << milestone_usage() << "\n";
    return 0;
  }
  try {
    const auto project = require_current_project_payload(paths);
    const auto project_id = json_string(project, "project_id");
    const auto card_id =
        resolve_card_reference(paths, project_id, options.card_reference, CardReferenceScope::Live);

    if (options.action == MilestoneAction::Remove) {
      const auto payload = milestone_api_request(
          paths,
          boost::beast::http::verb::delete_,
          "/cards/" + url_encode_component(card_id) + "/milestones/" +
              url_encode_component(options.value),
          boost::beast::http::status::ok,
          nlohmann::json::object()
      );
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else if (payload.at("data").value("removed", false)) {
        std::cout << "Removed milestone " << options.value << " from " << display_card_id(card_id)
                  << ".\n";
      } else {
        std::cout << "Milestone " << options.value << " was not present on "
                  << display_card_id(card_id) << ".\n";
      }
      return 0;
    }

    if (options.action == MilestoneAction::Edit) {
      nlohmann::json body = nlohmann::json::object();
      std::optional<ParsedMilestoneWhen> start;
      std::optional<ParsedMilestoneWhen> end;
      if (options.start.has_value()) {
        start = parse_cli_milestone_when(*options.start);
        body["start_at"] = start->epoch_seconds;
      }
      if (options.end.has_value()) {
        end = parse_cli_milestone_when(*options.end);
        body["end_at"] = end->epoch_seconds;
      } else if (options.clear_end) {
        body["end_at"] = nullptr;
      }

      if (start.has_value() && end.has_value() && start->kind != end->kind) {
        throw CliError(
            "invalid_milestone_time",
            "Milestone start and end must both be dates or both be timed.",
            {{"start", *options.start}, {"end", *options.end}},
            "Milestone start and end must both be dates or both be timed."
        );
      }
      std::optional<MilestoneWhenKind> supplied_time_kind;
      if (start.has_value())
        supplied_time_kind = start->kind;
      else if (end.has_value())
        supplied_time_kind = end->kind;
      if (options.all_day && supplied_time_kind == MilestoneWhenKind::Timed) {
        throw CliError(
            "invalid_milestone_time",
            "--all-day requires YYYY-MM-DD values.",
            {},
            "--all-day requires YYYY-MM-DD values."
        );
      }
      if (options.timed && supplied_time_kind == MilestoneWhenKind::AllDay) {
        throw CliError(
            "invalid_milestone_time",
            "--timed requires RFC 3339 values with an explicit offset.",
            {},
            "--timed requires RFC 3339 values with an explicit offset."
        );
      }
      if (options.all_day)
        body["all_day"] = true;
      else if (options.timed)
        body["all_day"] = false;
      else if (start.has_value())
        body["all_day"] = start->kind == MilestoneWhenKind::AllDay;

      if (options.kind.has_value())
        body["kind"] = *options.kind;
      else if (options.clear_kind)
        body["kind"] = nullptr;
      if (options.description.has_value())
        body["description"] = *options.description;
      else if (options.clear_description)
        body["description"] = nullptr;

      const auto payload = milestone_api_request(
          paths,
          boost::beast::http::verb::patch,
          "/cards/" + url_encode_component(card_id) + "/milestones/" +
              url_encode_component(options.value),
          boost::beast::http::status::ok,
          body
      );
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        const auto& data = payload.at("data");
        std::cout << "Updated milestone " << json_string(data, "milestone_id") << " on "
                  << display_card_id(card_id) << " at " << milestone_range(data) << ".\n";
      }
      return 0;
    }

    const auto start = parse_cli_milestone_when(options.value);
    const bool all_day = start.kind == MilestoneWhenKind::AllDay;
    if (options.all_day && !all_day) {
      throw CliError(
          "invalid_milestone_time",
          "--all-day requires a YYYY-MM-DD value.",
          {{"value", options.value}},
          "--all-day requires a YYYY-MM-DD value."
      );
    }
    nlohmann::json body{{"start_at", start.epoch_seconds}, {"all_day", all_day}};
    if (options.end.has_value()) {
      const auto end = parse_cli_milestone_when(*options.end);
      if (end.kind != start.kind) {
        throw CliError(
            "invalid_milestone_time",
            "Milestone start and end must both be dates or both be timed.",
            {{"start", options.value}, {"end", *options.end}},
            "Milestone start and end must both be dates or both be timed."
        );
      }
      if (end.epoch_seconds < start.epoch_seconds) {
        throw CliError(
            "invalid_milestone_time",
            "Milestone end must not be before its start.",
            {{"start", options.value}, {"end", *options.end}},
            "Milestone end must not be before its start."
        );
      }
      body["end_at"] = end.epoch_seconds;
    }
    if (options.kind.has_value()) body["kind"] = *options.kind;
    if (options.description.has_value()) body["description"] = *options.description;

    const auto payload = milestone_api_request(
        paths,
        boost::beast::http::verb::post,
        "/cards/" + url_encode_component(card_id) + "/milestones",
        boost::beast::http::status::created,
        body
    );
    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      const auto& data = payload.at("data");
      std::cout << "Added milestone " << json_string(data, "milestone_id") << " to "
                << display_card_id(card_id) << " at " << milestone_range(data) << ".\n";
    }
    return 0;
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to update milestone: ") + ex.what());
  }
}

int command_calendar(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto options = parse_calendar_options(argc, argv);
  if (options.help) {
    std::cout << calendar_usage() << "\n";
    return 0;
  }
  try {
    const auto project = require_current_project_payload(paths);
    const auto project_id = json_string(project, "project_id");
    const auto range = resolve_cli_calendar_range(options.from, options.to);
    const auto target = "/calendar?project_id=" + url_encode_component(project_id) +
                        "&from=" + std::to_string(range.from) + "&to=" + std::to_string(range.to);
    const auto payload = milestone_api_request(
        paths,
        boost::beast::http::verb::get,
        target,
        boost::beast::http::status::ok
    );
    if (options.json_output)
      std::cout << payload.dump(2) << "\n";
    else
      print_calendar(payload.at("data"));
    return 0;
  } catch (const CliError&) {
    throw;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to read calendar: ") + ex.what());
  }
}

} // namespace holder::cli
