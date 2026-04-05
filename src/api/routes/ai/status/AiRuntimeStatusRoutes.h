#pragma once

#include "llm/LocalModelRunner.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes::ai::status {

bool handle_ai_runtime_status_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    holder::llm::RunnerRegistry* runner_registry);

inline bool handle_ai_runtime_status_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    holder::llm::LocalModelRunner* runner) {
  holder::llm::RunnerRegistry runner_registry(runner);
  return handle_ai_runtime_status_routes(path, req, res, db, &runner_registry);
}

inline bool handle_ai_runtime_status_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    std::nullptr_t) {
  return handle_ai_runtime_status_routes(path, req, res, db, static_cast<holder::llm::RunnerRegistry*>(nullptr));
}

} // namespace holder::api::routes::ai::status
