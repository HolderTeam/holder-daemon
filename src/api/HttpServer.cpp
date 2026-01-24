#include "api/HttpServer.h"

#include "api/Listener.h"
#include "core/ServerInfo.h"

#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace holder::api {
namespace {

namespace http = boost::beast::http;

} // namespace

HttpServer::HttpServer(std::string bind,
                       unsigned short port,
                       holder::store::Db& db,
                       std::string auth_token,
                       holder::store::CardStore* card_store,
                       holder::index::FtsIndexer* fts)
    : bind_(std::move(bind)),
      port_(port),
      db_(db),
      auth_token_(std::move(auth_token)),
      router_(),
      card_store_(card_store),
      fts_(fts) {
  router_.add(http::verb::get, "/health",
              [this](const Router::Request&, Router::Response& res) {
                bool db_ok = true;
                try {
                  db_.exec("SELECT 1;");
                } catch (...) {
                  db_ok = false;
                }

                const auto uptime_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - started_at_)
                                           .count();

                nlohmann::json data;
                data["db_ok"] = db_ok;
                data["uptime_ms"] = uptime_ms;
                data["api_version"] = "0.1";
                data["server_version"] = CARD_SERVER_VERSION;
                data["pid"] = holder::core::current_pid();

                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = data;

                http::response<http::string_body> response{http::status::ok, 11};
                response.set(http::field::content_type, "application/json");
                response.keep_alive(false);
                response.body() = payload.dump();
                response.prepare_payload();
                res = std::move(response);
              });
}

HttpServer::~HttpServer() = default;

HttpServer::BoundInfo HttpServer::start() {
  started_at_ = std::chrono::steady_clock::now();
  listener_ = std::make_unique<Listener>(bind_, port_, db_, auth_token_, router_, started_at_, card_store_, fts_);
  const auto bound = listener_->start();
  return BoundInfo{bound.bind, bound.port};
}

void HttpServer::run(const holder::core::SignalHandler& signals) {
  if (!listener_) {
    throw std::runtime_error("HttpServer::start must be called before run.");
  }
  listener_->run(signals);
}

} // namespace holder::api
