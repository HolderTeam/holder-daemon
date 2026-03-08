#pragma once

#include "index/FtsIndexer.h"
#include "platform/Db.h"

#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes::ai::messages {

bool handle_ai_message_capture_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::store::Db& db,
    holder::index::FtsIndexer* fts,
    const std::function<std::string()>& uuid_v4);

} // namespace holder::api::routes::ai::messages
