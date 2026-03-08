#pragma once

#include "platform/Db.h"

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::routes::ai::providers {

bool handle_ai_provider_credential_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::store::Db& db);

} // namespace holder::api::routes::ai::providers
