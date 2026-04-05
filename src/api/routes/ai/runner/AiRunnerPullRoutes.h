#pragma once

#include "api/routes/ai/AiRunnerRoutes.h"
#include "llm/RunnerRegistry.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes::ai::runner {

RunnerRouteDispatchResult handle_ai_runner_pull_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::llm::RunnerRegistry* runner_registry);

} // namespace holder::api::routes::ai::runner
