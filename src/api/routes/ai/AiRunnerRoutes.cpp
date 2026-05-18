#include "api/routes/ai/AiRunnerRoutes.h"

#include "ai/AiRunnerRepo.h"
#include "api/routes/ai/runner/AiRunnerPullEventRoutes.h"
#include "api/routes/ai/runner/AiRunnerPullRoutes.h"
#include "api/support/HttpResponses.h"
#include "api/support/LocalModelRouting.h"
#include "api/support/Time.h"
#include "llm/RunnerModelRef.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <optional>

namespace holder::api::routes {
namespace {

namespace http = boost::beast::http;

std::optional<std::string> validated_runner_base_url(
    const nlohmann::json& body,
    bool required,
    std::string* error
) {
  if (!body.contains("base_url") || body.at("base_url").is_null()) {
    if (required) {
      if (error) *error = "Missing base_url.";
    }
    return std::nullopt;
  }

  const std::string value = holder::api::support::trim_ascii(body.at("base_url").get<std::string>()
  );
  if (value.empty()) {
    if (error) *error = "base_url cannot be empty.";
    return std::nullopt;
  }
  if (value.rfind("http://", 0) != 0) {
    if (error) *error = "base_url must use http://host:port format.";
    return std::nullopt;
  }
  const auto host_port = value.substr(std::string("http://").size());
  const auto colon = host_port.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon == host_port.size() - 1) {
    if (error) *error = "base_url must use http://host:port format.";
    return std::nullopt;
  }
  return value;
}

nlohmann::json local_model_to_json(const holder::llm::LocalModel& model) {
  return {
      {"name", model.name},
      {"model_ref", nullptr},
      {"digest", model.digest},
      {"size", model.size},
      {"modified_at", model.modified_at},
  };
}

nlohmann::json local_model_to_json(
    const holder::llm::LocalModel& model,
    const std::string& runner_id
) {
  auto item = local_model_to_json(model);
  item["runner_id"] = runner_id;
  item["model_ref"] = holder::llm::make_runner_model_ref(runner_id, model.name);
  return item;
} // LCOV_EXCL_LINE

nlohmann::json pull_job_to_json(
    const holder::llm::RunnerPullJob& job,
    const std::string& runner_id
) {
  return {
      {"job_id", job.job_id},
      {"runner_id", runner_id},
      {"model", job.model},
      {"status", job.status},
      {"updated_at", job.updated_at},
      {"error", job.error},
      {"progress",
       {{"completed", job.progress.completed},
        {"total", job.progress.total},
        {"percent", job.progress.percent},
        {"stage", job.progress.stage}}},
  };
}

nlohmann::json runner_runtime_to_json(
    const std::string& runner_id,
    holder::llm::RunnerClient* client
) {
  nlohmann::json runtime;
  runtime["configured"] = client != nullptr;
  if (client == nullptr) {
    runtime["available"] = false;
    runtime["spawn_attempted"] = false;
    runtime["last_checked"] = 0;
    runtime["version"] = nullptr;
    runtime["error"] = "runner_not_configured";
    runtime["models"] = nlohmann::json::array();
    runtime["pulls"] = nlohmann::json::array();
    return runtime;
  }

  const auto status = client->status();
  runtime["available"] = status.available;
  runtime["spawn_attempted"] = status.spawn_attempted;
  runtime["last_checked"] = status.last_checked;
  runtime["version"] = status.version.empty() ? nlohmann::json(nullptr)
                                              : nlohmann::json(status.version);
  runtime["error"] = status.error.empty() ? nlohmann::json(nullptr) : nlohmann::json(status.error);
  runtime["models"] = nlohmann::json::array();
  for (const auto& model : status.models) {
    runtime["models"].push_back(local_model_to_json(model, runner_id));
  }
  runtime["pulls"] = nlohmann::json::array();
  for (const auto& pull : client->list_pulls()) {
    runtime["pulls"].push_back(pull_job_to_json(pull, runner_id));
  }
  return runtime;
}

nlohmann::json runner_to_json(
    const holder::model::AiRunner& runner,
    holder::llm::RunnerRegistry* runner_registry
) {
  nlohmann::json item;
  item["runner_id"] = runner.runner_id;
  item["name"] = runner.name;
  item["kind"] = runner.kind;
  item["base_url"] = runner.base_url.has_value() ? nlohmann::json(runner.base_url.value())
                                                 : nlohmann::json(nullptr);
  item["source"] = runner.source;
  item["enabled"] = runner.enabled;
  item["created_at"] = runner.created_at;
  item["updated_at"] = runner.updated_at;
  item["runtime"] = runner_runtime_to_json(
      runner.runner_id,
      runner_registry ? runner_registry->get_client(runner.runner_id) : nullptr
  );
  return item;
} // LCOV_EXCL_LINE

bool handle_ai_runner_crud_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    holder::platform::Db& db,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string()>& uuid_v4
) {
  if (path == "/ai/runners" && req.method() == http::verb::get) {
    try {
      nlohmann::json runners = nlohmann::json::array();
      if (runner_registry != nullptr) {
        for (const auto& runner : runner_registry->list_runners()) {
          runners.push_back(runner_to_json(runner, runner_registry));
        }
      }
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {{"runners", runners}};
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path == "/ai/runners" && req.method() == http::verb::post) {
    try {
      const auto body = nlohmann::json::parse(req.body());
      if (!body.contains("name") || !body.contains("kind") || body.at("name").is_null() ||
          body.at("kind").is_null()) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "Missing name or kind."
        );
        return true;
      }

      const std::string name = support::trim_ascii(body.at("name").get<std::string>());
      const std::string kind = support::trim_ascii(body.at("kind").get<std::string>());
      if (name.empty()) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "name cannot be empty."
        );
        return true;
      }
      if (kind != "ollama") {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "Unsupported runner kind."
        );
        return true;
      }

      std::string base_url_error;
      const auto base_url = validated_runner_base_url(body, true, &base_url_error);
      if (!base_url.has_value()) {
        res = support::error_response(http::status::bad_request, "bad_request", base_url_error);
        return true;
      }

      const long long now = support::now_epoch_seconds();
      holder::model::AiRunner runner;
      runner.runner_id = "manual-" + uuid_v4();
      runner.name = name;
      runner.kind = kind;
      runner.base_url = base_url;
      runner.source = "manual";
      runner.enabled = !body.contains("enabled") || body.at("enabled").is_null()
                           ? true
                           : body.at("enabled").get<bool>();
      runner.created_at = now;
      runner.updated_at = now;

      holder::ai::AiRunnerRepo repo(db);
      repo.upsert(runner);
      if (runner_registry != nullptr) {
        runner_registry->refresh();
      }

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = runner_to_json(runner, runner_registry);
      res = support::json_response(http::status::created, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (path.rfind("/ai/runners/", 0) != 0) {
    return false;
  }

  const std::string suffix = path.substr(std::string("/ai/runners/").size());
  if (suffix.empty()) {
    return false;
  }
  const auto slash = suffix.find('/');
  const std::string runner_id = slash == std::string::npos ? suffix : suffix.substr(0, slash);
  const std::string subresource = slash == std::string::npos ? std::string()
                                                             : suffix.substr(slash + 1);
  if (runner_id.empty()) {
    return false;
  }

  if (subresource == "retry" && req.method() == http::verb::post) {
    try {
      if (runner_registry == nullptr) {
        res = support::error_response(http::status::not_found, "not_found", "Runner not found.");
        return true;
      }
      auto* client = runner_registry->get_client(runner_id);
      const auto runner = runner_registry->get_runner(runner_id);
      if (!runner.has_value() || client == nullptr) {
        res =
            support::error_response(http::status::not_found, "not_found", "Runner not configured.");
        return true;
      }
      spdlog::info("AI runner retry requested runner_id=" + runner_id);
      (void)client->retry();
      runner_registry->refresh();

      const auto refreshed = runner_registry->get_runner(runner_id);
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = runner_to_json(refreshed.value_or(runner.value()), runner_registry);
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (!subresource.empty()) {
    return false;
  }

  if (req.method() == http::verb::get) {
    try {
      if (runner_registry == nullptr) {
        res = support::error_response(http::status::not_found, "not_found", "Runner not found.");
        return true;
      }
      const auto runner = runner_registry->get_runner(runner_id);
      if (!runner.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "Runner not found.");
        return true;
      }
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = runner_to_json(runner.value(), runner_registry);
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (req.method() == http::verb::patch) {
    try {
      if (runner_id == holder::llm::RunnerRegistry::kAutoLocalRunnerId) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "auto-local runner is not editable."
        );
        return true;
      }

      holder::ai::AiRunnerRepo repo(db);
      const auto existing = repo.get(runner_id);
      if (!existing.has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "Runner not found.");
        return true;
      }

      const auto body = nlohmann::json::parse(req.body());
      auto runner = existing.value();
      if (body.contains("name") && !body.at("name").is_null()) {
        runner.name = support::trim_ascii(body.at("name").get<std::string>());
        if (runner.name.empty()) {
          res = support::error_response(
              http::status::bad_request,
              "bad_request",
              "name cannot be empty."
          );
          return true;
        }
      }
      if (body.contains("enabled") && !body.at("enabled").is_null()) {
        runner.enabled = body.at("enabled").get<bool>();
      }
      if (body.contains("base_url")) {
        std::string base_url_error;
        const auto base_url = validated_runner_base_url(body, false, &base_url_error);
        if (!base_url.has_value() && !body.at("base_url").is_null()) {
          res = support::error_response(http::status::bad_request, "bad_request", base_url_error);
          return true;
        }
        runner.base_url = base_url;
      }
      runner.updated_at = support::now_epoch_seconds();
      repo.upsert(runner);
      if (runner_registry != nullptr) {
        runner_registry->refresh();
      }

      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = runner_to_json(runner, runner_registry);
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  if (req.method() == http::verb::delete_) {
    try {
      if (runner_id == holder::llm::RunnerRegistry::kAutoLocalRunnerId) {
        res = support::error_response(
            http::status::bad_request,
            "bad_request",
            "auto-local runner is not deletable."
        );
        return true;
      }

      holder::ai::AiRunnerRepo repo(db);
      if (!repo.get(runner_id).has_value()) {
        res = support::error_response(http::status::not_found, "not_found", "Runner not found.");
        return true;
      }
      repo.remove(runner_id);
      if (runner_registry != nullptr) {
        runner_registry->refresh();
      }
      nlohmann::json payload;
      payload["ok"] = true;
      payload["data"] = {{"runner_id", runner_id}};
      res = support::json_response(http::status::ok, payload);
    } catch (const std::exception& ex) {
      res = support::error_response(http::status::bad_request, "bad_request", ex.what());
    }
    return true;
  }

  return false;
}

} // namespace

RunnerRouteDispatchResult handle_ai_runner_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get
) {
  if (handle_ai_runner_crud_routes(path, req, res, db, runner_registry, uuid_v4)) {
    return {.handled = true, .streamed = false};
  }
  if (const auto out = ai::runner::handle_ai_runner_pull_event_routes(
          path,
          req,
          res,
          socket,
          runner_registry,
          param_get
      );
      out.handled) {
    return out;
  }
  return ai::runner::handle_ai_runner_pull_routes(path, req, res, runner_registry, param_get);
}

} // namespace holder::api::routes
