#pragma once

#include "llm/LocalModelRunner.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"

#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes::ai::status {

bool handle_ai_capabilities_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string(const std::string&)>& param_get);

inline bool handle_ai_capabilities_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string(const std::string&)>& param_get) {
  holder::llm::RunnerRegistry runner_registry(runner);
  return handle_ai_capabilities_routes(path, req, res, db, &runner_registry, param_get);
}

inline bool handle_ai_capabilities_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    std::nullptr_t,
    const std::function<std::string(const std::string&)>& param_get) {
  return handle_ai_capabilities_routes(
      path, req, res, db, static_cast<holder::llm::RunnerRegistry*>(nullptr), param_get);
}

} // namespace holder::api::routes::ai::status
