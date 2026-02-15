#pragma once

#include "llm/LocalModelRunner.h"

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
    holder::llm::LocalModelRunner* runner);

} // namespace holder::api::routes
