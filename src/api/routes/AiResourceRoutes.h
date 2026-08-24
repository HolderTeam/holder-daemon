#pragma once

#include "platform/Db.h"
#include "privacy/SecretStore.h"
#include "git/GitOps.h"

#include <boost/beast/http.hpp>
#include <boost/asio/ip/tcp.hpp>

#include <functional>
#include <string>

namespace holder::api::routes {

// Listener shutdown calls this before destroying the serialized Git executor used by imports.
void wait_for_asset_import_jobs();

bool handle_ai_resource_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    holder::platform::Db& db,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get,
    holder::privacy::SecretStore* secret_store = nullptr,
    holder::git::GitOps* git_ops = nullptr,
    boost::asio::ip::tcp::socket* socket = nullptr,
    bool* streamed = nullptr
);

} // namespace holder::api::routes
