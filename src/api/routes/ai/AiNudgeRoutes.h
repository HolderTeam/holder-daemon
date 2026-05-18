#pragma once

#include "ai/NudgeService.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes {

bool handle_ai_nudge_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::ai::NudgeService* nudge_service
);

} // namespace holder::api::routes
