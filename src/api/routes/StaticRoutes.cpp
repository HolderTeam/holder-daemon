#include "api/routes/StaticRoutes.h"

#include "api/support/PathDiscovery.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <yaml-cpp/yaml.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

http::response<http::string_body> text_response(http::status status,
                                                std::string body,
                                                std::string content_type) {
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, std::move(content_type));
  res.keep_alive(false);
  res.body() = std::move(body);
  res.prepare_payload();
  return res;
} // LCOV_EXCL_LINE

nlohmann::json yaml_to_json(const YAML::Node& node) {
  if (!node || node.IsNull()) {
    return nullptr;
  }
  if (node.IsScalar()) {
    const std::string value = node.Scalar();
    if (value == "true") return true;
    if (value == "false") return false;
    char* end = nullptr;
    const auto integer = std::strtoll(value.c_str(), &end, 10);
    if (end != value.c_str() && *end == '\0') return integer;
    end = nullptr;
    const auto floating = std::strtod(value.c_str(), &end);
    if (end != value.c_str() && *end == '\0') return floating;
    return value;
  }
  if (node.IsSequence()) {
    nlohmann::json out = nlohmann::json::array();
    for (const auto& item : node) {
      out.push_back(yaml_to_json(item));
    }
    return out;
  }
  if (node.IsMap()) {
    nlohmann::json out = nlohmann::json::object();
    for (const auto& pair : node) {
      out[pair.first.as<std::string>()] = yaml_to_json(pair.second);
    }
    return out;
  }
  return nullptr; // LCOV_EXCL_LINE
}

bool serve_yaml_as_json(const std::filesystem::path& path,
                        std::string not_found_message,
                        http::response<http::string_body>& res) {
  const auto content = support::read_file(path);
  if (!content.has_value()) {
    res = text_response(http::status::not_found,
                        std::move(not_found_message),
                        "text/plain; charset=utf-8");
    return true;
  }
  try {
    const YAML::Node root = YAML::Load(content.value());
    const auto json = yaml_to_json(root);
    res = text_response(http::status::ok, json.dump(2), "application/json");
    return true;
  } catch (const std::exception&) {
    res = text_response(http::status::internal_server_error,
                        "Failed to parse YAML.",
                        "text/plain; charset=utf-8");
    return true;
  }
}

} // namespace

bool handle_static_routes(const std::string& path,
                          const http::request<http::string_body>& req,
                          http::response<http::string_body>& res) {
  if (!(path == "/openapi.yaml" || path == "/ai_catalog.yaml" || path == "/ai_catalog.json" ||
        path == "/git_providers.yaml" || path == "/git_providers.json" ||
        path == "/docs" || path.rfind("/docs/", 0) == 0)) {
    return false;
  }

  if (req.method() != http::verb::get) {
    res = text_response(http::status::method_not_allowed,
                        "Method not allowed.",
                        "text/plain; charset=utf-8");
    return true;
  }

  if (path == "/openapi.yaml") {
    const auto openapi_path = support::find_openapi_path();
    if (!openapi_path.has_value()) {
      res = text_response(http::status::not_found,
                          "openapi.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    const auto content = support::read_file(openapi_path.value());
    if (!content.has_value()) {
      res = text_response(http::status::not_found,
                          "openapi.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    res = text_response(http::status::ok,
                        content.value(),
                        support::content_type_for_extension(openapi_path->extension().string()));
    return true;
  }

  if (path == "/ai_catalog.yaml") {
    const auto ai_catalog_path = support::find_ai_catalog_path();
    if (!ai_catalog_path.has_value()) {
      res = text_response(http::status::not_found,
                          "ai_catalog.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    const auto content = support::read_file(ai_catalog_path.value());
    if (!content.has_value()) {
      res = text_response(http::status::not_found,
                          "ai_catalog.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    res = text_response(http::status::ok,
                        content.value(),
                        support::content_type_for_extension(ai_catalog_path->extension().string()));
    return true;
  }
  if (path == "/ai_catalog.json") {
    const auto ai_catalog_path = support::find_ai_catalog_path();
    if (!ai_catalog_path.has_value()) {
      res = text_response(http::status::not_found,
                          "ai_catalog.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    return serve_yaml_as_json(ai_catalog_path.value(), "ai_catalog.yaml not found.", res);
  }

  if (path == "/git_providers.yaml") {
    const auto git_providers_path = support::find_git_providers_path();
    if (!git_providers_path.has_value()) {
      res = text_response(http::status::not_found,
                          "git_providers.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    const auto content = support::read_file(git_providers_path.value());
    if (!content.has_value()) {
      res = text_response(http::status::not_found,
                          "git_providers.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    res = text_response(http::status::ok,
                        content.value(),
                        support::content_type_for_extension(git_providers_path->extension().string()));
    return true;
  }
  if (path == "/git_providers.json") {
    const auto git_providers_path = support::find_git_providers_path();
    if (!git_providers_path.has_value()) {
      res = text_response(http::status::not_found,
                          "git_providers.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    return serve_yaml_as_json(git_providers_path.value(), "git_providers.yaml not found.", res);
  }

  const auto docs_root = support::find_docs_root();
  if (!docs_root.has_value()) {
    res = text_response(http::status::not_found,
                        "Docs assets not found.",
                        "text/plain; charset=utf-8");
    return true;
  }

  std::string rel = "index.html";
  if (path.rfind("/docs/", 0) == 0) {
    rel = path.substr(std::string("/docs/").size());
    if (rel.empty()) rel = "index.html";
  }
  std::filesystem::path rel_path(rel);
  if (!support::is_safe_relpath(rel_path)) {
    res = text_response(http::status::not_found,
                        "Not found.",
                        "text/plain; charset=utf-8");
    return true;
  }

  const auto full_path = docs_root.value() / rel_path;
  const auto content = support::read_file(full_path);
  if (!content.has_value()) {
    res = text_response(http::status::not_found,
                        "Not found.",
                        "text/plain; charset=utf-8");
    return true;
  }
  res = text_response(http::status::ok,
                      content.value(),
                      support::content_type_for_extension(full_path.extension().string()));
  return true;
}

} // namespace holder::api::routes
