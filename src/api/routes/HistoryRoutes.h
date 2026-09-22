#pragma once

#include "platform/Db.h"
#include "privacy/PrivacyError.h"

namespace holder::card {
class CardStore;
}

#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes {

boost::beast::http::response<boost::beast::http::string_body> history_privacy_error_response(
    const holder::privacy::PrivacyError& error
);

bool handle_history_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    const std::function<std::string(const std::string&)>& param_get,
    holder::card::CardStore* card_store = nullptr
);

} // namespace holder::api::routes
