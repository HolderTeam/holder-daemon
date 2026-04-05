#pragma once

#include "api/routes/ai/AiRunnerRoutes.h"
#include "llm/LocalModelRunner.h"
#include "llm/RunnerRegistry.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes::ai::runner {

RunnerRouteDispatchResult handle_ai_runner_pull_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::llm::RunnerRegistry* runner_registry);

inline RunnerRouteDispatchResult handle_ai_runner_pull_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::llm::LocalModelRunner* runner) {
  holder::llm::RunnerRegistry runner_registry(runner);
  return handle_ai_runner_pull_routes(path, req, res, &runner_registry);
}

inline RunnerRouteDispatchResult handle_ai_runner_pull_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    std::nullptr_t) {
  return handle_ai_runner_pull_routes(path, req, res, static_cast<holder::llm::RunnerRegistry*>(nullptr));
}

} // namespace holder::api::routes::ai::runner
