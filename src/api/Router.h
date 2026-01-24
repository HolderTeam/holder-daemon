#pragma once

#include <boost/beast/http.hpp>

#include <functional>
#include <string>
#include <unordered_map>

namespace holder::api {

class Router {
public:
  using Request = boost::beast::http::request<boost::beast::http::string_body>;
  using Response = boost::beast::http::response<boost::beast::http::string_body>;
  using Handler = std::function<void(const Request&, Response&)>;

  void add(boost::beast::http::verb method, std::string target, Handler handler);
  bool dispatch(const Request& req, Response& res) const;

private:
  struct Key {
    boost::beast::http::verb method{};
    std::string target;

    bool operator==(const Key& other) const {
      return method == other.method && target == other.target;
    }
  };

  struct KeyHash {
    std::size_t operator()(const Key& key) const;
  };

  std::unordered_map<Key, Handler, KeyHash> routes_;
};

} // namespace holder::api
