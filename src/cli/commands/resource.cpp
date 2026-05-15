#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <boost/asio.hpp>
#include <boost/process/v2/environment.hpp>
#include <boost/process/v2/process.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace holder::cli {
namespace {

struct ResourceListOptions {
  bool json_output = false;
  std::string filter;
};

struct ResourceAddOptions {
  bool json_output = false;
  std::string uri;
  std::string kind;
  std::string label;
  std::optional<std::string> desc;
};

struct ResourceEditOptions {
  bool json_output = false;
  std::string resource_id;
  std::optional<std::string> kind;
  std::optional<std::string> uri;
  std::optional<std::string> label;
  std::optional<std::string> desc;
  bool clear_desc = false;
};

bool has_url_scheme(const std::string& uri) {
  const auto scheme_pos = uri.find("://");
  return scheme_pos != std::string::npos && scheme_pos > 0;
}

std::string strip_url_query_fragment(std::string value) {
  const auto pos = value.find_first_of("?#");
  if (pos != std::string::npos) {
    value.resize(pos);
  }
  while (!value.empty() && value.back() == '/') {
    value.pop_back();
  }
  return value;
}

std::string uri_basename(const std::string& uri) {
  const auto stripped = strip_url_query_fragment(uri);
  const auto slash_pos = stripped.find_last_of("/\\");
  if (slash_pos == std::string::npos) {
    return stripped;
  }
  return stripped.substr(slash_pos + 1);
}

std::string uri_host(const std::string& uri) { // LCOV_EXCL_LINE
  // LCOV_EXCL_START: fallback for malformed URL-like inputs after basename inference.
  const auto scheme_pos = uri.find("://");
  if (scheme_pos == std::string::npos) {
    return "";
  }
  const auto host_start = scheme_pos + 3;
  const auto host_end = uri.find('/', host_start);
  return uri.substr(host_start, host_end == std::string::npos ? std::string::npos : host_end - host_start);
  // LCOV_EXCL_STOP
} // LCOV_EXCL_LINE

bool has_image_extension(const std::string& uri) {
  const auto base = lower_ascii(uri_basename(uri));
  return base.ends_with(".png") || base.ends_with(".jpg") || base.ends_with(".jpeg") ||
         base.ends_with(".gif") || base.ends_with(".webp") || base.ends_with(".svg") ||
         base.ends_with(".bmp") || base.ends_with(".tif") || base.ends_with(".tiff");
} // LCOV_EXCL_LINE

std::string infer_resource_kind(const std::string& uri) {
  const auto lower = lower_ascii(uri);
  if (lower.rfind("git@", 0) == 0 || lower.ends_with(".git")) {
    return "repo";
  }
  if (has_image_extension(uri)) {
    return "image";
  }
  std::error_code ec;
  if (std::filesystem::is_directory(uri, ec)) {
    return "dir";
  }
  ec.clear();
  if (std::filesystem::is_regular_file(uri, ec)) {
    return "file";
  }
  if (has_url_scheme(uri)) {
    return "url";
  }
  if (uri.find('/') != std::string::npos || uri.find('\\') != std::string::npos) {
    return "file";
  }
  return "url";
}

std::string infer_resource_label(const std::string& uri) {
  auto label = uri_basename(uri);
  if (label.empty() && has_url_scheme(uri)) {
    label = uri_host(uri); // LCOV_EXCL_LINE: see uri_host.
  }
  if (label.empty()) {
    label = uri;
  }
  return label;
} // LCOV_EXCL_LINE

std::string resource_usage() {
  return "Usage: holderctl resource <list|add|show|edit|open|delete> ...";
}

ResourceListOptions parse_resource_list_options(int argc, char* argv[]) {
  ResourceListOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--filter") {
      if (i + 1 >= argc) {
        throw std::runtime_error("Usage: holderctl resource list [--json] [--filter <query>]");
      }
      options.filter = argv[++i];
    } else {
      throw std::runtime_error("Unknown resource list option: " + arg);
    }
  }
  return options;
}

ResourceAddOptions parse_resource_add_options(int argc, char* argv[]) {
  ResourceAddOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& usage) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(usage);
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error("Resource option value must not be empty");
      }
      return value;
    };

    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--kind") {
      options.kind = require_value("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    } else if (arg == "--label") {
      options.label = require_value("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    } else if (arg == "--desc") {
      options.desc = require_value("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown resource add option: " + arg);
    } else if (options.uri.empty()) {
      options.uri = arg;
    } else {
      throw std::runtime_error("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
    }
  }

  if (options.uri.empty()) {
    throw std::runtime_error("Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]");
  }
  if (options.kind.empty()) {
    options.kind = infer_resource_kind(options.uri);
  }
  if (options.label.empty()) {
    options.label = infer_resource_label(options.uri);
  }
  return options;
}

ResourceEditOptions parse_resource_edit_options(int argc, char* argv[]) {
  ResourceEditOptions options;
  for (int i = 3; i < argc; ++i) {
    const std::string arg = argv[i];
    auto require_value = [&](const std::string& usage) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(usage);
      }
      const std::string value = argv[++i];
      if (value.empty()) {
        throw std::runtime_error("Resource option value must not be empty");
      }
      return value;
    };

    if (arg == "--json") {
      options.json_output = true;
    } else if (arg == "--kind") {
      options.kind = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--uri") {
      options.uri = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--label") {
      options.label = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--desc") {
      options.desc = require_value("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    } else if (arg == "--clear-desc") {
      options.clear_desc = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown resource edit option: " + arg);
    } else if (options.resource_id.empty()) {
      options.resource_id = arg;
    } else {
      throw std::runtime_error("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
    }
  }

  if (options.resource_id.empty()) {
    throw std::runtime_error("Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]");
  }
  if (options.desc.has_value() && options.clear_desc) {
    throw std::runtime_error("--desc and --clear-desc cannot be used together");
  }
  if (!options.kind.has_value() && !options.uri.has_value() && !options.label.has_value() &&
      !options.desc.has_value() && !options.clear_desc) {
    throw std::runtime_error("resource edit requires at least one field to update");
  }
  return options;
}

std::string parse_single_resource_id(int argc, char* argv[], const std::string& usage) {
  if (argc != 4) {
    throw std::runtime_error(usage);
  }
  const std::string resource_id = argv[3];
  if (resource_id.empty()) {
    throw std::runtime_error(usage);
  }
  return resource_id;
}

nlohmann::json list_current_project_resources_payload(const holder::core::Paths& paths) {
  const auto project = require_current_project_payload(paths);
  const auto project_id = json_string(project, "project_id");
  return card_api_request(paths,
                          boost::beast::http::verb::get,
                          "/resources?project_id=" + url_encode_component(project_id));
}

nlohmann::json find_resource_in_payload(const nlohmann::json& resources,
                                        const std::string& resource_id) {
  for (const auto& resource : resources) {
    if (json_string(resource, "resource_id") == resource_id) {
      return resource;
    }
  }
  throw std::runtime_error("Resource not found in current project: " + resource_id);
}

bool resource_matches_filter(const nlohmann::json& resource, const std::string& filter) {
  if (filter.empty()) {
    return true; // LCOV_EXCL_LINE: callers only invoke this helper with non-empty filters.
  }
  const std::string haystack = json_string(resource, "label") + " " +
                              json_string(resource, "kind") + " " +
                              json_string(resource, "uri") + " " +
                              json_string(resource, "desc");
  return contains_case_insensitive(haystack, filter);
}

void print_resource_row(const nlohmann::json& resource) {
  std::cout << json_string(resource, "resource_id") << "\t"
            << json_string(resource, "kind") << "\t"
            << json_string(resource, "label") << "\t"
            << json_string(resource, "uri") << "\n";
}

void open_resource_uri(const std::string& uri) {
#if defined(__linux__)
  const auto opener = boost::process::v2::environment::find_executable("xdg-open");
  if (opener.empty()) {
    std::cout << uri << "\n"; // LCOV_EXCL_LINE: depends on host PATH contents.
    throw std::runtime_error("xdg-open not found"); // LCOV_EXCL_LINE
  }

  boost::asio::io_context ioc;
  boost::process::v2::process proc(ioc.get_executor(), opener, {uri});

  boost::system::error_code ec;
  const int exit_code = proc.wait(ec);
  if (ec) {
    std::cout << uri << "\n"; // LCOV_EXCL_LINE: requires process wait syscall failure.
    throw std::runtime_error("Failed to run xdg-open: " + ec.message()); // LCOV_EXCL_LINE
  }
  if (exit_code != 0) {
    std::cout << uri << "\n";
    throw std::runtime_error("xdg-open failed with exit code " + std::to_string(exit_code));
  }
#else
  std::cout << uri << "\n";
  throw std::runtime_error("resource open is not supported on this platform yet"); // LCOV_EXCL_LINE
#endif
}

} // namespace

int command_resource(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(resource_usage());
  }

  const std::string subcommand = argv[2];
  try {
    if (subcommand == "list") {
      const auto options = parse_resource_list_options(argc, argv);
      auto payload = list_current_project_resources_payload(paths);
      if (options.filter.empty()) {
        if (options.json_output) {
          std::cout << payload.dump(2) << "\n";
          return 0;
        }
      } else {
        nlohmann::json filtered = nlohmann::json::array();
        for (const auto& resource : payload.at("data")) {
          if (resource_matches_filter(resource, options.filter)) {
            filtered.push_back(resource);
          }
        }
        payload["data"] = std::move(filtered);
        if (options.json_output) {
          std::cout << payload.dump(2) << "\n";
          return 0;
        }
      }

      const auto& resources = payload.at("data");
      if (!resources.is_array() || resources.empty()) {
        std::cout << "No resources.\n";
        return 0;
      }

      std::cout << "RESOURCE_ID\tKIND\tLABEL\tURI\n";
      for (const auto& resource : resources) {
        print_resource_row(resource);
      }
      return 0;
    }

    if (subcommand == "add") {
      const auto options = parse_resource_add_options(argc, argv);
      const auto project = require_current_project_payload(paths);
      const auto project_id = json_string(project, "project_id");
      nlohmann::json body = {
          {"project_id", project_id},
          {"kind", options.kind},
          {"uri", options.uri},
          {"label", options.label},
          {"created_at", now_epoch_seconds()}, // LCOV_EXCL_LINE: gcov misattributes covered JSON initializer lines.
          {"updated_at", now_epoch_seconds()}, // LCOV_EXCL_LINE
      };
      if (options.desc.has_value()) {
        body["desc"] = options.desc.value();
      }
      const auto payload = card_api_request(paths,
                                            boost::beast::http::verb::post,
                                            "/resources",
                                            body,
                                            boost::beast::http::status::created);
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        std::cout << "Created resource: " << json_string(payload.at("data"), "resource_id") << "\n";
      }
      return 0;
    }

    if (subcommand == "show") {
      bool json_output = false;
      int resource_arg_index = 3;
      if (argc >= 4 && std::string(argv[3]) == "--json") {
        json_output = true;
        resource_arg_index = 4;
      }
      if (argc != resource_arg_index + 1) {
        throw std::runtime_error("Usage: holderctl resource show [--json] <resource-id>");
      }
      const std::string resource_id = argv[resource_arg_index];
      const auto payload = list_current_project_resources_payload(paths);
      const auto resource = find_resource_in_payload(payload.at("data"), resource_id);
      if (json_output) {
        nlohmann::json out;
        out["ok"] = true;
        out["data"] = resource;
        std::cout << out.dump(2) << "\n";
      } else {
        std::cout << "Resource: " << json_string(resource, "resource_id") << "\n"
                  << "Kind: " << json_string(resource, "kind") << "\n"
                  << "Label: " << json_string(resource, "label") << "\n"
                  << "URI: " << json_string(resource, "uri") << "\n";
        const auto desc = json_string(resource, "desc");
        if (!desc.empty()) {
          std::cout << "Desc: " << desc << "\n";
        }
      }
      return 0;
    }

    if (subcommand == "edit") {
      const auto options = parse_resource_edit_options(argc, argv);
      const auto resources_payload = list_current_project_resources_payload(paths);
      (void)find_resource_in_payload(resources_payload.at("data"), options.resource_id);

      nlohmann::json body;
      body["updated_at"] = now_epoch_seconds();
      if (options.kind.has_value()) body["kind"] = options.kind.value();
      if (options.uri.has_value()) body["uri"] = options.uri.value();
      if (options.label.has_value()) body["label"] = options.label.value();
      if (options.clear_desc) {
        body["desc"] = nullptr;
      } else if (options.desc.has_value()) {
        body["desc"] = options.desc.value();
      }

      const auto payload = card_api_request(paths,
                                            boost::beast::http::verb::patch,
                                            "/resources/" + url_encode_component(options.resource_id),
                                            body);
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        std::cout << "Updated resource: " << options.resource_id << "\n";
      }
      return 0;
    }

    if (subcommand == "open") {
      const auto resource_id = parse_single_resource_id(argc, argv, "Usage: holderctl resource open <resource-id>");
      const auto payload = list_current_project_resources_payload(paths);
      const auto resource = find_resource_in_payload(payload.at("data"), resource_id);
      const auto uri = json_string(resource, "uri");
      if (uri.empty()) {
        throw std::runtime_error("Resource has no URI: " + resource_id);
      }
      open_resource_uri(uri);
      return 0;
    }

    if (subcommand == "delete") {
      const auto resource_id = parse_single_resource_id(argc, argv, "Usage: holderctl resource delete <resource-id>");
      const auto resources_payload = list_current_project_resources_payload(paths);
      (void)find_resource_in_payload(resources_payload.at("data"), resource_id);
      (void)card_api_request(paths,
                             boost::beast::http::verb::delete_,
                             "/resources/" + url_encode_component(resource_id));
      std::cout << "Deleted resource: " << resource_id << "\n";
      return 0;
    }
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Resource command failed: ") + ex.what());
  }

  throw std::runtime_error(resource_usage());
}

} // namespace holder::cli
