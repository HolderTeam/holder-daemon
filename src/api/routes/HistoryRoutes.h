#pragma once

#include "platform/Db.h"

namespace holder::card {
class CardStore;
}

#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes {

bool handle_history_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    const std::function<std::string(const std::string&)>& param_get,
    holder::card::CardStore* card_store = nullptr
);

} // namespace holder::api::routes
