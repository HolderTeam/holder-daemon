#pragma once

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace holder::api::support {

inline boost::beast::http::response<boost::beast::http::string_body> json_response(
    boost::beast::http::status status,
    const nlohmann::json& payload
) {
  namespace http = boost::beast::http;
  http::response<http::string_body> res{status, 11};
  res.set(http::field::content_type, "application/json");
  res.keep_alive(false);
  res.body() = payload.dump();
  res.prepare_payload();
  return res;
} // LCOV_EXCL_LINE

inline boost::beast::http::response<boost::beast::http::string_body> error_response(
    boost::beast::http::status status,
    std::string code,
    std::string message
) {
  nlohmann::json payload;
  payload["ok"] = false;
  payload["error"] = {{"code", std::move(code)}, {"message", std::move(message)}};
  return json_response(status, payload);
}

} // namespace holder::api::support
