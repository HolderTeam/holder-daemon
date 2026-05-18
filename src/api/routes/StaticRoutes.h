#pragma once

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes {

bool handle_static_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res
);

} // namespace holder::api::routes
