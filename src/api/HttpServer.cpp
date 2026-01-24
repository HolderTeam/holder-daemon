#include "api/HttpServer.h"

#include "core/ServerInfo.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <string>
#include <thread>
#include <stdexcept>
#include <utility>

namespace holder::api {
namespace {

namespace http = boost::beast::http;

constexpr auto kPollDelay = std::chrono::milliseconds(50);

bool is_authorized(const http::request<http::string_body>& req, const std::string& token) {
  const auto it = req.find(http::field::authorization);
  if (it == req.end()) return false;

  const auto value = it->value();
  const std::string auth(value.data(), value.size());
  constexpr char kPrefix[] = "Bearer ";
  if (auth.rfind(kPrefix, 0) != 0) return false;

  const std::string bearer = auth.substr(sizeof(kPrefix) - 1);
  return bearer == token;
}

http::response<http::string_body> json_response(http::status status,
                                                const nlohmann::json& payload) {
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, "application/json");
  res.keep_alive(false);
  res.body() = payload.dump();
  res.prepare_payload();
  return res;
}

http::response<http::string_body> error_response(http::status status,
                                                 std::string code,
                                                 std::string message) {
  nlohmann::json j;
  j["ok"] = false;
  j["error"] = {{"code", std::move(code)}, {"message", std::move(message)}};
  return json_response(status, j);
}

} // namespace

HttpServer::HttpServer(std::string bind,
                       unsigned short port,
                       holder::store::Db& db,
                       std::string auth_token)
    : acceptor_(ioc_),
      bind_(std::move(bind)),
      port_(port),
      db_(db),
      auth_token_(std::move(auth_token)) {}

HttpServer::BoundInfo HttpServer::start() {
  boost::system::error_code ec;
  const auto address = boost::asio::ip::make_address(bind_, ec);
  if (ec) {
    throw std::runtime_error("Invalid bind address: " + bind_ + " (" + ec.message() + ")");
  }

  tcp::endpoint endpoint(address, port_);
  acceptor_.open(endpoint.protocol(), ec);
  if (ec) throw std::runtime_error("acceptor.open failed: " + ec.message());

  acceptor_.set_option(boost::asio::socket_base::reuse_address(true), ec);
  if (ec) throw std::runtime_error("acceptor.set_option failed: " + ec.message());

  acceptor_.bind(endpoint, ec);
  if (ec) throw std::runtime_error("acceptor.bind failed: " + ec.message());

  acceptor_.listen(boost::asio::socket_base::max_listen_connections, ec);
  if (ec) throw std::runtime_error("acceptor.listen failed: " + ec.message());

  acceptor_.non_blocking(true, ec);
  if (ec) throw std::runtime_error("acceptor.non_blocking failed: " + ec.message());

  started_at_ = std::chrono::steady_clock::now();

  const auto bound = acceptor_.local_endpoint(ec);
  if (ec) throw std::runtime_error("acceptor.local_endpoint failed: " + ec.message());

  return BoundInfo{bound.address().to_string(), bound.port()};
}

void HttpServer::run(const holder::core::SignalHandler& signals) {
  using http::verb;

  while (!signals.is_requested()) {
    boost::system::error_code ec;
    tcp::socket socket(ioc_);
    acceptor_.accept(socket, ec);
    if (ec) {
      if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again) {
        std::this_thread::sleep_for(kPollDelay);
        continue;
      }
      spdlog::error("accept failed: {}", ec.message());
      continue;
    }

    handle_session(socket);
  }
}

void HttpServer::handle_session(tcp::socket& socket) {
  namespace beast = boost::beast;

  beast::flat_buffer buffer;
  http::request<http::string_body> req;
  boost::system::error_code ec;

  http::read(socket, buffer, req, ec);
  if (ec == http::error::end_of_stream) {
    socket.shutdown(tcp::socket::shutdown_send, ec);
    return;
  }
  if (ec) {
    spdlog::warn("read failed: {}", ec.message());
    socket.shutdown(tcp::socket::shutdown_send, ec);
    return;
  }

  http::response<http::string_body> res;

  if (!is_authorized(req, auth_token_)) {
    res = error_response(http::status::unauthorized, "unauthorized", "Missing or invalid token.");
  } else if (req.method() == http::verb::get && req.target() == "/health") {
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

    res = json_response(http::status::ok, payload);
  } else {
    res = error_response(http::status::not_found, "not_found", "Route not found.");
  }

  http::write(socket, res, ec);
  if (ec) {
    spdlog::warn("write failed: {}", ec.message());
  }

  spdlog::info("HTTP {} {} -> {}", req.method_string(), req.target(), res.result_int());

  socket.shutdown(tcp::socket::shutdown_send, ec);
}

} // namespace holder::api
