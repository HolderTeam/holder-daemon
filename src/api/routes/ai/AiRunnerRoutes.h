#pragma once

#include "llm/LocalModelRunner.h"
#include "llm/RunnerRegistry.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes {

struct RunnerRouteDispatchResult {
  bool handled = false;
  bool streamed = false;
};

RunnerRouteDispatchResult handle_ai_runner_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::llm::RunnerRegistry* runner_registry);

inline RunnerRouteDispatchResult handle_ai_runner_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::llm::LocalModelRunner* runner) {
  holder::llm::RunnerRegistry runner_registry(runner);
  return handle_ai_runner_routes(path, req, res, socket, &runner_registry);
}

inline RunnerRouteDispatchResult handle_ai_runner_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    std::nullptr_t) {
  return handle_ai_runner_routes(path, req, res, socket, static_cast<holder::llm::RunnerRegistry*>(nullptr));
}

} // namespace holder::api::routes
