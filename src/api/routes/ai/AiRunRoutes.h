#pragma once

#include "index/FtsIndexer.h"
#include "llm/LocalModelRunner.h"
#include "llm/RunnerRegistry.h"
#include "platform/Db.h"
#include "privacy/SecretStore.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>

#include <functional>
#include <string>

namespace holder::api::routes {

struct RouteDispatchResult {
  bool handled = false;
  bool streamed = false;
};

RouteDispatchResult handle_ai_run_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::privacy::SecretStore* secret_store,
    holder::llm::RunnerRegistry* runner_registry,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get);

inline RouteDispatchResult handle_ai_run_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::privacy::SecretStore* secret_store,
    holder::llm::LocalModelRunner* runner,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get) {
  holder::llm::RunnerRegistry runner_registry(runner);
  return handle_ai_run_routes(path, req, res, socket, db, fts, secret_store, &runner_registry, uuid_v4, param_get);
}

inline RouteDispatchResult handle_ai_run_routes(
    const std::string& path,
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    boost::beast::http::response<boost::beast::http::string_body>& res,
    boost::asio::ip::tcp::socket& socket,
    holder::platform::Db& db,
    holder::index::FtsIndexer* fts,
    holder::privacy::SecretStore* secret_store,
    std::nullptr_t,
    const std::function<std::string()>& uuid_v4,
    const std::function<std::string(const std::string&)>& param_get) {
  return handle_ai_run_routes(path,
                              req,
                              res,
                              socket,
                              db,
                              fts,
                              secret_store,
                              static_cast<holder::llm::RunnerRegistry*>(nullptr),
                              uuid_v4,
                              param_get);
}

} // namespace holder::api::routes
