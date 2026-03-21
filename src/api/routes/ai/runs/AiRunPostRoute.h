#pragma once

#include "api/routes/ai/AiRunRoutes.h"
#include "index/FtsIndexer.h"
#include "llm/LocalModelRunner.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes::ai::runs {

RouteDispatchResult handle_ai_runs_post_route(
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::privacy::SecretStore* secret_store,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4);

} // namespace holder::api::routes::ai::runs
