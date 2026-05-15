#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace holder::cli {
namespace {

struct ProjectNewOptions {
  bool json_output = false;
  bool use = false;
  std::string name;
  std::string privacy_mode = "encrypted_git";
  std::optional<std::string> remote_url;
};

std::string project_usage() {
  return "Usage: holderctl project new <name> [--plain|--encrypted] [--remote <url>] [--use] [--json]";
}

ProjectNewOptions parse_project_new_options(int argc, char* argv[]) {
  ProjectNewOptions options;
  std::vector<std::string> name_parts;
  bool privacy_set = false;

  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& option) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(project_usage());
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error(option + " must not be empty");
      }
      return value;
    };

    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--use") {
      options.use = true;
    } else if (arg == "--plain") {
      if (privacy_set && options.privacy_mode != "plain") {
        throw std::runtime_error("--plain and --encrypted cannot be used together");
      }
      options.privacy_mode = "plain";
      privacy_set = true;
    } else if (arg == "--encrypted") {
      if (privacy_set && options.privacy_mode != "encrypted_git") {
        throw std::runtime_error("--plain and --encrypted cannot be used together");
      }
      options.privacy_mode = "encrypted_git";
      privacy_set = true;
    } else if (arg == "--remote") {
      options.remote_url = require_value(arg);
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown project new option: " + arg);
    } else {
      name_parts.push_back(arg);
    }
  }

  for (const auto& part : name_parts) {
    if (!options.name.empty()) options.name += " ";
    options.name += part;
  }
  if (trim_ascii_whitespace(options.name).empty()) {
    throw std::runtime_error(project_usage());
  }
  return options;
}

} // namespace

int command_project(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(project_usage());
  }

  const std::string subcommand = argv[2];
  if (subcommand != "new") {
    throw std::runtime_error(project_usage());
  }

  const auto options = parse_project_new_options(argc, argv);
  nlohmann::json body = {
      {"name", options.name},
      {"privacy_mode", options.privacy_mode},
      {"created_at", now_epoch_seconds()}, // LCOV_EXCL_LINE: gcov misattributes covered JSON initializer lines.
      {"updated_at", now_epoch_seconds()}, // LCOV_EXCL_LINE
  };
  if (options.remote_url.has_value()) {
    body["git_remote_url"] = options.remote_url.value();
  }

  try {
    const auto payload = card_api_request(paths,
                                          boost::beast::http::verb::post,
                                          "/projects",
                                          body,
                                          boost::beast::http::status::created);
    const auto& project = payload.at("data");
    if (options.use) {
      write_holderctl_config(paths, json_string(project, "project_id"));
    }

    if (options.json_output) {
      std::cout << payload.dump(2) << "\n";
    } else {
      std::cout << "Created project: " << json_string(project, "project_id") << "\n";
      if (options.use) {
        std::cout << "Current project: " << json_string(project, "name") << " ("
                  << json_string(project, "project_id") << ")\n";
      }
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to create project: ") + ex.what());
  }
}

int command_projects(const holder::core::Paths& paths, int argc, char* argv[]) {
  bool json_output = false;
  bool include_count = false;
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      json_output = true;
    } else if (arg == "--count") {
      include_count = true;
    } else {
      throw std::runtime_error("Unknown projects option: " + arg);
    }
  }

  try {
    const auto payload = list_projects_payload(paths, include_count);

    if (json_output) {
      std::cout << payload.dump(2) << "\n";
      return 0;
    }

    const auto& projects = payload.at("data");
    if (!projects.is_array() || projects.empty()) {
      std::cout << "No projects.\n";
      return 0;
    }

    if (include_count) {
      std::cout << "PROJECT_ID\tNAME\tCARDS\tROOT_CARDS\tROOT\n";
    } else {
      std::cout << "PROJECT_ID\tNAME\tROOT\n";
    }
    for (const auto& project : projects) {
      std::cout << json_string(project, "project_id") << "\t"
                << json_string(project, "name") << "\t";
      if (include_count) {
        std::cout << project.value("card_count", 0) << "\t"
                  << project.value("root_card_count", 0) << "\t";
      }
      std::cout << json_string(project, "root_path") << "\n";
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Failed to list projects: ") + ex.what());
  }
}

int command_use(const holder::core::Paths& paths, int argc, char* argv[]) {
  const auto payload = list_projects_payload(paths, false);
  if (argc == 2) {
    const auto project = find_home_project(payload.at("data"));
    reset_holderctl_config(paths);
    std::cout << "Current project: " << json_string(project, "name") << " ("
              << json_string(project, "project_id") << ")\n";
    return 0;
  }

  if (argc != 3) {
    throw std::runtime_error("Usage: holderctl use <project-id-or-name>");
  }

  const std::string query = argv[2];
  const auto project = resolve_project(payload.at("data"), query);
  const auto project_id = json_string(project, "project_id");
  if (is_home_project(project)) {
    reset_holderctl_config(paths);
  } else {
    write_holderctl_config(paths, project_id);
  }

  std::cout << "Current project: " << json_string(project, "name") << " (" << project_id << ")\n";
  return 0;
}

int command_current(const holder::core::Paths& paths, int argc) {
  if (argc != 2) {
    throw std::runtime_error("current does not take options");
  }

  const auto current_project_id = read_current_project_id(paths);
  const auto payload = list_projects_payload(paths, false);
  const auto project = find_project_by_id(payload.at("data"), current_project_id);

  std::cout << "Current project: " << json_string(project, "name") << " ("
            << current_project_id << ")\n"
            << "Root: " << json_string(project, "root_path") << "\n";
  return 0;
}

} // namespace holder::cli
