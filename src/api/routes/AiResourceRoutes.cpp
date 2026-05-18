#include "api/routes/AiResourceRoutes.h"
#include "api/support/HttpResponses.h"
#include "api/support/Time.h"

#include "resource/ResourceRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

} // namespace

bool handle_ai_resource_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get
) {
  if (path == "/resources" && req.method() == http::verb::get) {
    const std::string project_id = param_get("project_id");
    if (project_id.empty()) {
      res =
          support::error_response(http::status::bad_request, "bad_request", "Missing project_id.");
    } else {
      try {
        holder::resource::ResourceRepo repo(db);
        const auto resources = repo.list(project_id);
        nlohmann::json data = nlohmann::json::array();
        for (const auto& resource : resources) {
          nlohmann::json item;
          item["resource_id"] = resource.resource_id;
          item["project_id"] = resource.project_id;
          item["kind"] = resource.kind;
          item["uri"] = resource.uri;
          item["label"] = resource.label;
          item["desc"] = resource.desc.has_value() ? nlohmann::json(resource.desc.value())
                                                   : nlohmann::json(nullptr);
          item["created_at"] = resource.created_at;
          item["updated_at"] = resource.updated_at;
          data.push_back(std::move(item));
        }
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = data;
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    }
    return true;
  }

  if (path == "/resources" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("project_id") || !body.contains("kind") || !body.contains("uri") ||
          !body.contains("label")) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "Missing required fields."
        );
      } else {
        holder::model::Resource resource;
        if (body.contains("resource_id") && !body.at("resource_id").is_null()) {
          resource.resource_id = body.at("resource_id").get<std::string>();
        }
        if (resource.resource_id.empty()) {
          resource.resource_id = uuid_v4();
        }
        resource.project_id = body.at("project_id").get<std::string>();
        resource.kind = body.at("kind").get<std::string>();
        resource.uri = body.at("uri").get<std::string>();
        resource.label = body.at("label").get<std::string>();
        if (body.contains("desc") && !body.at("desc").is_null()) {
          resource.desc = body.at("desc").get<std::string>();
        }
        if (body.contains("created_at") && !body.at("created_at").is_null()) {
          resource.created_at = body.at("created_at").get<long long>();
        }
        if (body.contains("updated_at") && !body.at("updated_at").is_null()) {
          resource.updated_at = body.at("updated_at").get<long long>();
        }
        if (resource.created_at <= 0) {
          resource.created_at = support::now_epoch_seconds();
        }
        if (resource.updated_at <= 0) {
          resource.updated_at = resource.created_at;
        }

        holder::resource::ResourceRepo repo(db);
        repo.add(resource);

        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"resource_id", resource.resource_id}};
        res = support::json_response(http::status::created, payload);
      }
    } catch (const std::exception& ex) {
      const std::string msg = ex.what();
      if (msg.rfind("conflict:", 0) == 0) {
        res = support::error_response(http::status::conflict, "conflict", msg);
      } else {
        res = support::error_response(http::status::bad_request, "bad_request", msg);
      }
    }
    return true;
  }

  if (path.rfind("/resources/", 0) == 0) {
    const std::string resource_id = path.substr(std::string("/resources/").size());
    if (resource_id.empty()) {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    } else if (req.method() == http::verb::patch) {
      try {
        const auto body = nlohmann::json::parse(req.body());
        if (!body.contains("updated_at")) {
          res = support::error_response(
              http::status::bad_request,
              "bad_request",
              "Missing updated_at."
          );
        } else {
          holder::resource::ResourceRepo repo(db);
          const auto existing = repo.get(resource_id);
          if (!existing.has_value()) {
            res = support::error_response(
                http::status::not_found,
                "not_found",
                "Resource not found."
            );
          } else {
            auto resource = existing.value();
            if (body.contains("kind") && !body.at("kind").is_null()) {
              resource.kind = body.at("kind").get<std::string>();
            }
            if (body.contains("uri") && !body.at("uri").is_null()) {
              resource.uri = body.at("uri").get<std::string>();
            }
            if (body.contains("label") && !body.at("label").is_null()) {
              resource.label = body.at("label").get<std::string>();
            }
            if (body.contains("desc")) {
              if (body.at("desc").is_null()) {
                resource.desc.reset();
              } else {
                resource.desc = body.at("desc").get<std::string>();
              }
            }
            resource.updated_at = body.at("updated_at").get<long long>();
            repo.update(resource);

            nlohmann::json payload;
            payload["ok"] = true;
            payload["data"] = {{"resource_id", resource_id}};
            res = support::json_response(http::status::ok, payload);
          }
        }
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else if (req.method() == http::verb::delete_) {
      try {
        holder::resource::ResourceRepo repo(db);
        repo.remove(resource_id);
        nlohmann::json payload;
        payload["ok"] = true;
        payload["data"] = {{"resource_id", resource_id}};
        res = support::json_response(http::status::ok, payload);
      } catch (const std::exception& ex) {
        res = support::error_response(http::status::bad_request, "bad_request", ex.what());
      }
    } else {
      res = support::error_response(http::status::not_found, "not_found", "Route not found.");
    }
    return true;
  }

  return false;
}

} // namespace holder::api::routes
