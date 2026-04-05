#include "api/HttpServer.h"

#include "api/Listener.h"
#include "api/support/Health.h"
#include "platform/Paths.h"

#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

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
                       holder::platform::Db& db,
                       std::string auth_token,
                       holder::card::CardStore* card_store,
                       holder::index::FtsIndexer* fts)
    : HttpServer(std::move(bind),
                 port,
                 db,
                 std::move(auth_token),
                 card_store,
                 fts,
                 nullptr,
                 static_cast<holder::llm::RunnerRegistry*>(nullptr)) {}

HttpServer::HttpServer(std::string bind,
                       unsigned short port,
                       holder::platform::Db& db,
                       std::string auth_token,
                       holder::card::CardStore* card_store,
                       holder::index::FtsIndexer* fts,
                       holder::git::GitOps* git_ops)
    : HttpServer(std::move(bind),
                 port,
                 db,
                 std::move(auth_token),
                 card_store,
                 fts,
                 git_ops,
                 static_cast<holder::llm::RunnerRegistry*>(nullptr)) {}

HttpServer::HttpServer(std::string bind,
                       unsigned short port,
                       holder::platform::Db& db,
                       std::string auth_token,
                       holder::card::CardStore* card_store,
                       holder::index::FtsIndexer* fts,
                       holder::git::GitOps* git_ops,
                       holder::llm::LocalModelRunner* runner)
    : bind_(std::move(bind)),
      port_(port),
      db_(db),
      auth_token_(std::move(auth_token)),
      router_(),
      owned_runner_registry_(std::make_unique<holder::llm::RunnerRegistry>(runner)),
      runner_registry_(owned_runner_registry_.get()),
      nudge_service_(db, runner_registry_),
      secret_store_(holder::privacy::make_default_secret_store(holder::core::Paths::resolve("holder").server_dir())),
      card_store_(card_store),
      fts_(fts),
      git_ops_(git_ops) {
  router_.add(http::verb::get, "/health",
              [this](const Router::Request&, Router::Response& res) {
                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = support::build_health_data(db_, started_at_);

                http::response<http::string_body> response{http::status::ok, 11};
                response.set(http::field::content_type, "application/json");
                response.keep_alive(false);
                response.body() = payload.dump();
                response.prepare_payload();
                res = std::move(response);
              });
}

HttpServer::HttpServer(std::string bind,
                       unsigned short port,
                       holder::platform::Db& db,
                       std::string auth_token,
                       holder::card::CardStore* card_store,
                       holder::index::FtsIndexer* fts,
                       holder::git::GitOps* git_ops,
                       holder::llm::RunnerRegistry* runner_registry)
    : bind_(std::move(bind)),
      port_(port),
      db_(db),
      auth_token_(std::move(auth_token)),
      router_(),
      owned_runner_registry_(nullptr),
      runner_registry_(runner_registry),
      nudge_service_(db, runner_registry),
      secret_store_(holder::privacy::make_default_secret_store(holder::core::Paths::resolve("holder").server_dir())),
      card_store_(card_store),
      fts_(fts),
      git_ops_(git_ops) {
  router_.add(http::verb::get, "/health",
              [this](const Router::Request&, Router::Response& res) {
                nlohmann::json payload;
                payload["ok"] = true;
                payload["data"] = support::build_health_data(db_, started_at_);

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
  listener_ = std::make_unique<Listener>(bind_,
                                         port_,
                                         db_,
                                         auth_token_,
                                         router_,
                                         started_at_,
                                         card_store_,
                                         fts_,
                                         &nudge_service_,
                                         secret_store_.get(),
                                         git_ops_,
                                         runner_registry_);
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
