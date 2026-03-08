#pragma once

#include "llm/LocalModelRunner.h"
#include "platform/Db.h"

#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes {

bool handle_ai_status_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::store::Db& db,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string(const std::string&)>& param_get);

} // namespace holder::api::routes
