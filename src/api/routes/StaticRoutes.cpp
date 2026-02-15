#include "api/routes/StaticRoutes.h"

#include "api/support/PathDiscovery.h"

#include <boost/beast/http.hpp>

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
}

} // namespace

bool handle_static_routes(const std::string& path,
                          const http::request<http::string_body>& req,
                          http::response<http::string_body>& res) {
  if (!(path == "/openapi.yaml" || path == "/models.yaml" || path == "/cloudproviders.yaml" ||
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

  if (path == "/models.yaml") {
    const auto models_path = support::find_models_path();
    if (!models_path.has_value()) {
      res = text_response(http::status::not_found,
                          "models.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    const auto content = support::read_file(models_path.value());
    if (!content.has_value()) {
      res = text_response(http::status::not_found,
                          "models.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    res = text_response(http::status::ok,
                        content.value(),
                        support::content_type_for_extension(models_path->extension().string()));
    return true;
  }

  if (path == "/cloudproviders.yaml") {
    const auto cloudproviders_path = support::find_cloudproviders_path();
    if (!cloudproviders_path.has_value()) {
      res = text_response(http::status::not_found,
                          "cloudproviders.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    const auto content = support::read_file(cloudproviders_path.value());
    if (!content.has_value()) {
      res = text_response(http::status::not_found,
                          "cloudproviders.yaml not found.",
                          "text/plain; charset=utf-8");
      return true;
    }
    res = text_response(http::status::ok,
                        content.value(),
                        support::content_type_for_extension(cloudproviders_path->extension().string()));
    return true;
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
