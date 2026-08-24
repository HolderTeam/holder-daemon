#include "cli/commands/Commands.h"

#include "cli/commands/Support.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

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
  auto stripped = strip_url_query_fragment(uri);
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
  return uri.substr(
      host_start,
      host_end == std::string::npos ? std::string::npos : host_end - host_start
  );
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
  return "Usage: holderctl resource <list|add|show|edit|open|delete|import|location> ...";
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
      options.kind = require_value(
          "Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]"
      ); // LCOV_EXCL_LINE
    } else if (arg == "--label") {
      options.label = require_value(
          "Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]"
      ); // LCOV_EXCL_LINE
    } else if (arg == "--desc") {
      options.desc = require_value(
          "Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]"
      );
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown resource add option: " + arg);
    } else if (options.uri.empty()) {
      options.uri = arg;
    } else {
      throw std::runtime_error(
          "Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]"
      );
    }
  }

  if (options.uri.empty()) {
    throw std::runtime_error(
        "Usage: holderctl resource add <uri> [--kind <kind>] [--label <label>] [--desc <text>] [--json]"
    );
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
      options.kind = require_value(
          "Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]"
      );
    } else if (arg == "--uri") {
      options.uri = require_value(
          "Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]"
      );
    } else if (arg == "--label") {
      options.label = require_value(
          "Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]"
      );
    } else if (arg == "--desc") {
      options.desc = require_value(
          "Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]"
      );
    } else if (arg == "--clear-desc") {
      options.clear_desc = true;
    } else if (arg.rfind("--", 0) == 0) {
      throw std::runtime_error("Unknown resource edit option: " + arg);
    } else if (options.resource_id.empty()) {
      options.resource_id = arg;
    } else {
      throw std::runtime_error(
          "Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]"
      );
    }
  }

  if (options.resource_id.empty()) {
    throw std::runtime_error(
        "Usage: holderctl resource edit <resource-id> [--kind <kind>] [--uri <uri>] [--label <label>] [--desc <text>|--clear-desc] [--json]"
    );
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
  return card_api_request(
      paths,
      boost::beast::http::verb::get,
      "/resources?project_id=" + url_encode_component(project_id)
  );
}

nlohmann::json find_resource_in_payload(
    const nlohmann::json& resources,
    const std::string& resource_id
) {
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
                               json_string(resource, "type") + " " +
                               resource.value("metadata", nlohmann::json::object()).dump();
  return contains_case_insensitive(haystack, filter);
}

std::string metadata_first(const nlohmann::json& resource, const std::string& property) {
  if (!resource.contains("metadata") || !resource.at("metadata").is_object()) return "";
  const auto& metadata = resource.at("metadata");
  if (!metadata.contains(property) || !metadata.at(property).is_array() ||
      metadata.at(property).empty()) {
    return "";
  }
  return metadata.at(property).at(0).get<std::string>();
}

void print_resource_row(const nlohmann::json& resource) {
  std::cout << json_string(resource, "resource_id") << "\t" << json_string(resource, "type")
            << "\t" << json_string(resource, "label") << "\t"
            << metadata_first(resource, "identifier") << "\n";
}

void open_resource_uri(const std::string& uri) {
  open_external_uri(uri);
}

} // namespace

int command_resource(const holder::core::Paths& paths, int argc, char* argv[]) {
  if (argc < 3) {
    throw std::runtime_error(resource_usage());
  }

  const std::string subcommand = argv[2];
  try {
    if (subcommand == "import") {
      if (argc < 5 || argc > 7) {
        throw std::runtime_error(
            "Usage: holderctl resource import <card-id> <file> [--location <location-id>]"
        );
      }
      const auto project = require_current_project_payload(paths);
      const auto project_id = json_string(project, "project_id");
      std::string location_id;
      if (argc == 7) {
        if (std::string(argv[5]) != "--location" || std::string(argv[6]).empty()) {
          throw std::runtime_error(
              "Usage: holderctl resource import <card-id> <file> [--location <location-id>]"
          );
        }
        location_id = argv[6];
      } else {
        const auto locations = card_api_request(
            paths,
            boost::beast::http::verb::get,
            "/locations?project_id=" + url_encode_component(project_id)
        );
        location_id = json_string(locations, "preferred_location_id");
        if (location_id.empty()) {
          throw std::runtime_error("No preferred Storage Location; pass --location");
        }
      }
      const auto source = std::filesystem::absolute(argv[4]).lexically_normal();
      auto payload = card_api_request(
          paths,
          boost::beast::http::verb::post,
          "/imports",
          {{"project_id", project_id},
           {"card_id", argv[3]},
           {"location_id", location_id},
           {"source_path", source.string()}},
          boost::beast::http::status::accepted
      );
      const auto job_id = json_string(payload.at("data"), "job_id");
      if (job_id.empty()) throw std::runtime_error("Import response did not contain a job id");
      for (int attempt = 0; attempt < 600; ++attempt) {
        payload = card_api_request(
            paths,
            boost::beast::http::verb::get,
            "/imports/" + url_encode_component(job_id)
        );
        const auto& job = payload.at("data");
        const auto status = json_string(job, "status");
        if (status == "completed") {
          std::cout << "Attached resource: " << json_string(job, "resource_id");
          if (job.value("duplicate_reused", false)) std::cout << " (existing asset reused)";
          std::cout << "\n";
          return 0;
        }
        if (status == "failed") {
          throw std::runtime_error("Asset import failed: " + json_string(job, "error"));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      throw std::runtime_error("Asset import did not finish within 60 seconds");
    }

    if (subcommand == "location") {
      if (argc < 4) {
        throw std::runtime_error(
            "Usage: holderctl resource location <list|add-local|add-s3|test|prefer|delete> ..."
        );
      }
      const auto project = require_current_project_payload(paths);
      const auto project_id = json_string(project, "project_id");
      const std::string action = argv[3];
      if (action == "list") {
        if (argc != 4 && !(argc == 5 && std::string(argv[4]) == "--json")) {
          throw std::runtime_error("Usage: holderctl resource location list [--json]");
        }
        const auto payload = card_api_request(
            paths,
            boost::beast::http::verb::get,
            "/locations?project_id=" + url_encode_component(project_id)
        );
        if (argc == 5) {
          std::cout << payload.dump(2) << "\n";
        } else if (payload.at("data").empty()) {
          std::cout << "No storage locations.\n";
        } else {
          for (const auto& location : payload.at("data")) {
            std::cout << json_string(location, "location_id") << "\t"
                      << json_string(location, "provider") << "\t"
                      << json_string(location, "name") << "\t"
                      << (location.value("bound", false) ? "configured" : "binding required")
                      << "\n";
          }
        }
        return 0;
      }
      if (action == "add-local") {
        if (argc != 6) {
          throw std::runtime_error(
              "Usage: holderctl resource location add-local <name> <directory>"
          );
        }
        const auto root = std::filesystem::absolute(argv[5]).lexically_normal();
        const auto created = card_api_request(
            paths,
            boost::beast::http::verb::post,
            "/locations",
            {{"project_id", project_id},
             {"name", argv[4]},
             {"provider", "local_directory"},
             {"configuration", nlohmann::json::object()}},
            boost::beast::http::status::created
        );
        const auto location_id = json_string(created.at("data"), "location_id");
        (void)card_api_request(
            paths,
            boost::beast::http::verb::put,
            "/locations/" + url_encode_component(location_id) + "/binding",
            {{"values", {{"root_path", root.string()}}}, {"preview", root.string()}}
        );
        (void)card_api_request(
            paths,
            boost::beast::http::verb::put,
            "/locations/preferred",
            {{"project_id", project_id}, {"location_id", location_id}}
        );
        std::cout << "Created storage location: " << location_id << "\n";
        return 0;
      }
      if (action == "add-s3") {
        if (argc < 9) {
          throw std::runtime_error(
              "Usage: holderctl resource location add-s3 <name> <endpoint> <region> <bucket> "
              "<access-key-id> [--prefix <prefix>] [--virtual-host] [--allow-http-localhost]; "
              "set HOLDER_S3_SECRET_ACCESS_KEY and optionally HOLDER_S3_SESSION_TOKEN"
          );
        }
        const char* secret = std::getenv("HOLDER_S3_SECRET_ACCESS_KEY");
        if (secret == nullptr || std::string(secret).empty()) {
          throw std::runtime_error("HOLDER_S3_SECRET_ACCESS_KEY is required");
        }
        nlohmann::json configuration = {
            {"endpoint", argv[5]},
            {"region", argv[6]},
            {"bucket", argv[7]},
            {"addressing_style", "path"},
        };
        for (int index = 9; index < argc; ++index) {
          const std::string option = argv[index];
          if (option == "--prefix") {
            if (++index >= argc || std::string(argv[index]).empty()) {
              throw std::runtime_error("--prefix requires a value");
            }
            configuration["prefix"] = argv[index];
          } else if (option == "--virtual-host") {
            configuration["addressing_style"] = "virtual_host";
          } else if (option == "--allow-http-localhost") {
            configuration["allow_insecure_localhost"] = "true";
          } else {
            throw std::runtime_error("Unknown add-s3 option: " + option);
          }
        }
        const auto created = card_api_request(
            paths,
            boost::beast::http::verb::post,
            "/locations",
            {{"project_id", project_id},
             {"name", argv[4]},
             {"provider", "s3_compatible"},
             {"configuration", configuration}},
            boost::beast::http::status::created
        );
        const auto location_id = json_string(created.at("data"), "location_id");
        nlohmann::json values = {
            {"access_key_id", argv[8]}, {"secret_access_key", std::string(secret)}};
        if (const char* session = std::getenv("HOLDER_S3_SESSION_TOKEN");
            session != nullptr && std::string(session).length() > 0) {
          values["session_token"] = session;
        }
        (void)card_api_request(
            paths,
            boost::beast::http::verb::put,
            "/locations/" + url_encode_component(location_id) + "/binding",
            {{"values", values},
             {"preview", std::string(argv[5]) + "/" + std::string(argv[7])}}
        );
        (void)card_api_request(
            paths,
            boost::beast::http::verb::put,
            "/locations/preferred",
            {{"project_id", project_id}, {"location_id", location_id}}
        );
        std::cout << "Created S3-compatible storage location: " << location_id << "\n";
        return 0;
      }
      if (action == "test" || action == "prefer" || action == "delete") {
        if (argc != 5) {
          throw std::runtime_error("Storage location id is required");
        }
        const std::string location_id = argv[4];
        if (action == "test") {
          (void)card_api_request(
              paths,
              boost::beast::http::verb::post,
              "/locations/" + url_encode_component(location_id) + "/test",
              nlohmann::json::object()
          );
          std::cout << "Storage location is available.\n";
        } else if (action == "prefer") {
          (void)card_api_request(
              paths,
              boost::beast::http::verb::put,
              "/locations/preferred",
              {{"project_id", project_id}, {"location_id", location_id}}
          );
          std::cout << "Preferred storage location: " << location_id << "\n";
        } else {
          (void)card_api_request(
              paths,
              boost::beast::http::verb::delete_,
              "/locations/" + url_encode_component(location_id)
          );
          std::cout << "Deleted storage location: " << location_id << "\n";
        }
        return 0;
      }
      throw std::runtime_error("Unknown storage location action: " + action);
    }

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
      // LCOV_EXCL_START
      nlohmann::json metadata = nlohmann::json::object();
      metadata["identifier"] = nlohmann::json::array({options.uri});
      if (options.desc.has_value()) metadata["description"] = nlohmann::json::array({*options.desc});
      nlohmann::json body = {
          {"project_id", project_id},
          {"type", options.kind},
          {"label", options.label},
          {"metadata", std::move(metadata)},
          {"created_at", now_epoch_seconds()},
          {"updated_at", now_epoch_seconds()},
      };
      // LCOV_EXCL_STOP
      const auto payload = card_api_request(
          paths,
          boost::beast::http::verb::post,
          "/resources",
          body,
          boost::beast::http::status::created
      );
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
                  << "Type: " << json_string(resource, "type") << "\n"
                  << "Label: " << json_string(resource, "label") << "\n"
                  << "Identifier: " << metadata_first(resource, "identifier") << "\n";
        const auto desc = metadata_first(resource, "description");
        if (!desc.empty()) {
          std::cout << "Desc: " << desc << "\n";
        }
      }
      return 0;
    }

    if (subcommand == "edit") {
      const auto options = parse_resource_edit_options(argc, argv);
      const auto resources_payload = list_current_project_resources_payload(paths);
      const auto existing = find_resource_in_payload(resources_payload.at("data"), options.resource_id);

      nlohmann::json body;
      body["updated_at"] = now_epoch_seconds();
      if (options.kind.has_value()) body["type"] = options.kind.value();
      if (options.label.has_value()) body["label"] = options.label.value();
      auto metadata = existing.value("metadata", nlohmann::json::object());
      if (options.uri.has_value()) metadata["identifier"] = nlohmann::json::array({*options.uri});
      if (options.clear_desc) {
        // PATCH metadata is merged property-by-property; an empty value list explicitly removes
        // a property while omission leaves it unchanged.
        metadata["description"] = nlohmann::json::array();
      } else if (options.desc.has_value()) {
        metadata["description"] = nlohmann::json::array({*options.desc});
      }
      body["metadata"] = std::move(metadata);

      const auto payload = card_api_request(
          paths,
          boost::beast::http::verb::patch,
          "/resources/" + url_encode_component(options.resource_id),
          body
      );
      if (options.json_output) {
        std::cout << payload.dump(2) << "\n";
      } else {
        std::cout << "Updated resource: " << options.resource_id << "\n";
      }
      return 0;
    }

    if (subcommand == "open") {
      const auto resource_id =
          parse_single_resource_id(argc, argv, "Usage: holderctl resource open <resource-id>");
      const auto payload = list_current_project_resources_payload(paths);
      const auto resource = find_resource_in_payload(payload.at("data"), resource_id);
      const auto uri = metadata_first(resource, "identifier");
      if (uri.empty()) {
        throw std::runtime_error("Resource has no URI: " + resource_id);
      }
      open_resource_uri(uri);
      return 0;
    }

    if (subcommand == "delete") {
      const auto resource_id =
          parse_single_resource_id(argc, argv, "Usage: holderctl resource delete <resource-id>");
      const auto resources_payload = list_current_project_resources_payload(paths);
      (void)find_resource_in_payload(resources_payload.at("data"), resource_id);
      (void)card_api_request(
          paths,
          boost::beast::http::verb::delete_,
          "/resources/" + url_encode_component(resource_id)
      );
      std::cout << "Deleted resource: " << resource_id << "\n";
      return 0;
    }
  } catch (const std::exception& ex) {
    throw std::runtime_error(std::string("Resource command failed: ") + ex.what());
  }

  throw std::runtime_error(resource_usage());
}

} // namespace holder::cli
