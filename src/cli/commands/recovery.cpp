#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace holder::cli {
namespace {

struct RecoveryTokenOptions {
  std::string pin;
  std::filesystem::path out_path;
  std::filesystem::path in_path;
  std::string token;
};

std::string recovery_token_usage(const std::string& subcommand) {
  if (subcommand == "export") {
    return "Usage: holderctl recovery-token export --pin <pin> [--out <file>]";
  }
  if (subcommand == "import") {
    return "Usage: holderctl recovery-token import --pin <pin> (--file <file> | --token <token>)";
  }
  if (subcommand == "import-global") {
    return "Usage: holderctl recovery-token import-global --pin <pin> (--file <file> | --token <token>)";
  }
  return "Usage: holderctl recovery-token <export|import|import-global> ...";
}

RecoveryTokenOptions parse_recovery_token_options(const std::string& subcommand,
                                                  int argc,
                                                  char* argv[]) {
  RecoveryTokenOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& option) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(recovery_token_usage(subcommand));
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error(option + " must not be empty");
      }
      return value;
    };

    if (arg == "--pin") {
      options.pin = require_value(arg);
    } else if (arg == "--out" && subcommand == "export") {
      options.out_path = require_value(arg);
    } else if (arg == "--file" && (subcommand == "import" || subcommand == "import-global")) {
      options.in_path = require_value(arg);
    } else if (arg == "--token" && (subcommand == "import" || subcommand == "import-global")) {
      options.token = require_value(arg);
    } else {
      throw std::runtime_error("Unknown recovery-token option: " + arg);
    }
  }

  if (options.pin.empty()) {
    throw std::runtime_error(recovery_token_usage(subcommand));
  }

  if (subcommand == "export") {
    if (!options.in_path.empty() || !options.token.empty()) {
      throw std::runtime_error(recovery_token_usage(subcommand)); // LCOV_EXCL_LINE: parser rejects these options for export.
    }
  } else {
    const bool has_file = !options.in_path.empty();
    const bool has_token = !options.token.empty();
    if (has_file == has_token) {
      throw std::runtime_error(recovery_token_usage(subcommand));
    }
    if (!options.out_path.empty()) {
      throw std::runtime_error(recovery_token_usage(subcommand)); // LCOV_EXCL_LINE: parser rejects --out for import subcommands.
    }
  }

  return options;
}

std::string read_text_file_trimmed(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    throw std::runtime_error("Failed to open recovery token file: " + path.string());
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const auto token = trim_ascii_whitespace(buffer.str());
  if (token.empty()) {
    throw std::runtime_error("Recovery token file is empty: " + path.string());
  }
  return token;
}

void write_recovery_token_file(const std::filesystem::path& path, const std::string& token) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      throw std::runtime_error("Failed to open recovery token output file: " + path.string());
    }
    out << token << "\n";
  }
#ifndef _WIN32
  ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}

} // namespace

int command_recovery_token(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(recovery_token_usage(""));
  }

  const std::string subcommand = argv[2];
  if (subcommand != "export" && subcommand != "import" && subcommand != "import-global") {
    throw std::runtime_error(recovery_token_usage(""));
  }

  const auto options = parse_recovery_token_options(subcommand, argc, argv);
  try {
    if (subcommand == "export") {
      const auto current_project_id = read_current_project_id(paths);
      const auto projects_payload = list_projects_payload(paths, false);
      (void)find_project_by_id(projects_payload.at("data"), current_project_id);

      const auto payload = recovery_token_request(
          paths,
          boost::beast::http::verb::post,
          "/projects/" + url_encode_component(current_project_id) + "/recovery-token/export",
          {{"pin", options.pin}});
      const auto token = json_string(payload.at("data"), "recovery_token");
      if (token.empty()) {
        throw std::runtime_error("Recovery token export response did not include a token"); // LCOV_EXCL_LINE: protocol violation.
      }

      if (!options.out_path.empty()) {
        write_recovery_token_file(options.out_path, token);
        std::cout << "Recovery token exported: " << options.out_path.string() << "\n"
                  << "Project: " << current_project_id << "\n";
      } else {
        std::cout << token << "\n";
      }
      return 0;
    }

    const std::string token = !options.token.empty()
                                  ? trim_ascii_whitespace(options.token)
                                  : read_text_file_trimmed(options.in_path);
    if (token.empty()) {
      throw std::runtime_error("Recovery token must not be empty");
    }

    if (subcommand == "import") {
      const auto current_project_id = read_current_project_id(paths);
      const auto projects_payload = list_projects_payload(paths, false);
      (void)find_project_by_id(projects_payload.at("data"), current_project_id);

      const auto payload = recovery_token_request(
          paths,
          boost::beast::http::verb::post,
          "/projects/" + url_encode_component(current_project_id) + "/recovery-token/import",
          {{"pin", options.pin}, {"recovery_token", token}});
      std::cout << "Recovery token imported for project: "
                << json_string(payload.at("data"), "project_id", current_project_id) << "\n";
      return 0;
    }

    const auto payload = recovery_token_request(paths,
                                                boost::beast::http::verb::post,
                                                "/recovery-token/import",
                                                {{"pin", options.pin}, {"recovery_token", token}});
    const auto& data = payload.at("data");
    std::cout << "Recovery token imported for project: " << json_string(data, "project_id") << "\n"
              << "Project created: " << (data.value("project_created", false) ? "yes" : "no") << "\n" // LCOV_EXCL_LINE: gcov misattributes covered ostream chain lines.
              << "Remote hint: " << (data.value("remote_hint_present", false) ? "yes" : "no") << "\n" // LCOV_EXCL_LINE
              << "Remote configured: " << (data.value("remote_configured", false) ? "yes" : "no") << "\n" // LCOV_EXCL_LINE
              << "Pull status: " << json_string(data, "pull_status", "not_attempted") << "\n";
    if (data.contains("remote_error") && !data.at("remote_error").is_null()) {
      std::cout << "Remote error: " << data.at("remote_error").get<std::string>() << "\n"; // LCOV_EXCL_LINE: depends on optional git remote failure.
    }
    if (data.contains("pull_error") && !data.at("pull_error").is_null()) {
      std::cout << "Pull error: " << data.at("pull_error").get<std::string>() << "\n"; // LCOV_EXCL_LINE: depends on optional git pull failure.
    }
    return 0;
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Recovery token command failed: ") + ex.what());
  }
}

} // namespace holder::cli
