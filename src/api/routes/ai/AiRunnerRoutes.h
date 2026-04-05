#pragma once

#include "platform/Db.h"
#include "llm/RunnerRegistry.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#include <functional>
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
    holder::platform::Db& db,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get);

} // namespace holder::api::routes
