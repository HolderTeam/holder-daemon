#pragma once

#include "api/routes/ai/AiRunRoutes.h"
#include "platform/Db.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes::ai::runs {

RouteDispatchResult handle_ai_runs_list_route(
    const std::function<std::string(const std::string&)>& param_get,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::store::Db& db);

RouteDispatchResult handle_ai_runs_events_route(const std::string& path,
                                                boost::asio::ip::tcp::socket& socket,
                                                boost::beast::http::response<boost::beast::http::string_body>& res,
                                                holder::store::Db& db);

RouteDispatchResult handle_ai_runs_get_route(const std::string& path,
                                             boost::beast::http::response<boost::beast::http::string_body>& res,
                                             holder::store::Db& db);

} // namespace holder::api::routes::ai::runs
