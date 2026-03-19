#include "api/routes/ai/AiDispatch.h"

#include "api/routes/ai/AiMessageRoutes.h"
#include "api/routes/ai/AiNudgeRoutes.h"
#include "api/routes/ai/AiProviderRoutes.h"
#include "api/routes/ai/AiRunnerRoutes.h"
#include "api/routes/ai/AiRunRoutes.h"
#include "api/routes/ai/AiStatusRoutes.h"
#include "api/routes/ai/AiThreadRoutes.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes::ai {
namespace {

namespace http = boost::beast::http;

std::string segment_at(const std::string& path, std::size_t index) {
  if (path.empty() || path[0] != '/') return {};
  std::size_t cursor = 1;
  std::size_t current = 1;
  while (cursor <= path.size()) {
    const auto end = path.find('/', cursor);
    const auto seg_end = (end == std::string::npos) ? path.size() : end;
    if (current == index) {
      if (cursor >= seg_end) return {};
      return path.substr(cursor, seg_end - cursor);
    }
    if (end == std::string::npos) break;
    cursor = end + 1;
    ++current;
  }
  return {};
}

} // namespace

DispatchResult dispatch_ai_routes(
    const std::string& path,
    const http::request<http::string_body>& req,
    http::response<http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::ai::NudgeService* nudge_service,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param) {
  const std::string ai_resource = segment_at(path, 2);

  if (ai_resource == "capabilities" || ai_resource == "status" || ai_resource == "retry" ||
      ai_resource == "router") {
    if (handle_ai_status_routes(path, req, res, db, runner, param)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "providers") {
    if (handle_ai_provider_routes(path, req, res, db)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "nudges") {
    if (handle_ai_nudge_routes(path, req, res, nudge_service)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "runs") {
    if (const auto route_result =
            handle_ai_run_routes(path, req, res, socket, db, fts, runner, uuid_v4, param);
        route_result.handled) {
      return {.handled = true, .streamed = route_result.streamed};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "runner") {
    if (handle_ai_status_routes(path, req, res, db, runner, param)) {
      return {.handled = true, .streamed = false};
    }
    if (const auto route_result = handle_ai_runner_routes(path, req, res, socket, runner);
        route_result.handled) {
      return {.handled = true, .streamed = route_result.streamed};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "threads") {
    if (handle_ai_thread_routes(path, req, res, db, uuid_v4, param)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }
  if (ai_resource == "messages") {
    if (handle_ai_message_routes(path, req, res, db, fts, uuid_v4, param)) {
      return {.handled = true, .streamed = false};
    }
    return {.handled = false, .streamed = false};
  }

  return {.handled = false, .streamed = false};
}

} // namespace holder::api::routes::ai
