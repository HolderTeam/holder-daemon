#pragma once

#include "store/Db.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes {

bool handle_ai_provider_routes(const std::string& path,
                               const boost::beast::http::request<boost::beast::http::string_body>& req,
                               boost::beast::http::response<boost::beast::http::string_body>& res,
                               holder::store::Db& db);

} // namespace holder::api::routes
