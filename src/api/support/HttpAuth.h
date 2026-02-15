#pragma once

#include <boost/beast/http.hpp>

#include <string>

namespace holder::api::support {

inline bool is_authorized_bearer(
    const boost::beast::http::request<boost::beast::http::string_body>& req,
    const std::string& token) {
  namespace http = boost::beast::http;
  const auto it = req.find(http::field::authorization);
  if (it == req.end()) return false;

  const auto value = it->value();
  const std::string auth(value.data(), value.size());
  constexpr char kPrefix[] = "Bearer ";
  if (auth.rfind(kPrefix, 0) != 0) return false;

  const std::string bearer = auth.substr(sizeof(kPrefix) - 1);
  return bearer == token;
}

} // namespace holder::api::support
