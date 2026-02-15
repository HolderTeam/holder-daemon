#include "api/routes/ai/AiRunnerRoutes.h"

#include "api/routes/ai/runner/AiRunnerPullEventRoutes.h"
#include "api/routes/ai/runner/AiRunnerPullRoutes.h"

namespace holder::api::routes {

RunnerRouteDispatchResult handle_ai_runner_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::llm::LocalModelRunner* runner) {
  if (const auto out = ai::runner::handle_ai_runner_pull_event_routes(path, req, res, socket, runner);
      out.handled) {
    return out;
  }
  return ai::runner::handle_ai_runner_pull_routes(path, req, res, runner);
}

} // namespace holder::api::routes
