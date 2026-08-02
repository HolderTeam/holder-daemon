#pragma once

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/HttpServer.h"
#include "card/CardStore.h"
#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Project.h"
#include "platform/Db.h"
#include "platform/Signal.h"
#include "project/ProjectRepo.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace holder::test {

template <typename Socket>
inline void close_http_socket(Socket& socket) {
  boost::system::error_code ec;
  socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
  socket.close(ec);
}

inline std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  auto pattern = (base / "holder_http_test_XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');

#ifndef _WIN32
  char* created = ::mkdtemp(writable.data());
  if (created == nullptr) {
    throw std::runtime_error("mkdtemp failed creating holder_http_test temp dir");
  }
  return std::filesystem::path(created);
#else
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto suffix = std::to_string(
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
    );
    auto dir = base / ("holder_http_test_" + suffix);
    std::error_code ec;
    if (std::filesystem::create_directory(dir, ec)) {
      return dir;
    }
    if (ec && ec != std::errc::file_exists) {
      throw std::filesystem::filesystem_error("create_directory", dir, ec);
    }
  }
  throw std::runtime_error("failed to create unique holder_http_test temp dir");
#endif
}

inline nlohmann::json get_health(
    const std::string& bind,
    unsigned short port,
    const std::string& token
) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bind, std::to_string(port));

  boost::beast::tcp_stream stream(ioc);
  stream.expires_after(std::chrono::seconds(2));
  stream.connect(endpoints);

  http::request<http::string_body> req{http::verb::get, "/health", 11};
  req.set(http::field::host, bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);
  req.keep_alive(false);

  http::write(stream, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  close_http_socket(stream.socket());

  REQUIRE(res.result() == http::status::ok);
  return nlohmann::json::parse(res.body());
}

inline bool wait_for_http_listener(
    const std::string& bind,
    unsigned short port,
    std::chrono::milliseconds timeout = std::chrono::seconds(2)
) {
  using tcp = boost::asio::ip::tcp;

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      boost::asio::io_context ioc;
      tcp::resolver resolver(ioc);
      auto endpoints = resolver.resolve(bind, std::to_string(port));

      boost::beast::tcp_stream stream(ioc);
      stream.expires_after(std::chrono::milliseconds(200));
      stream.connect(endpoints);
      close_http_socket(stream.socket());
      return true;
    } catch (const std::exception&) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  return false;
}

inline bool wait_for_http_health_ready(
    const std::string& bind,
    unsigned short port,
    const std::string& token,
    std::chrono::milliseconds timeout = std::chrono::seconds(2)
) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      boost::asio::io_context ioc;
      tcp::resolver resolver(ioc);
      auto endpoints = resolver.resolve(bind, std::to_string(port));

      boost::beast::tcp_stream stream(ioc);
      stream.expires_after(std::chrono::milliseconds(200));
      stream.connect(endpoints);

      http::request<http::string_body> req{http::verb::get, "/health", 11};
      req.set(http::field::host, bind);
      req.set(http::field::user_agent, "holder-tests");
      req.set(http::field::authorization, "Bearer " + token);
      req.keep_alive(false);
      http::write(stream, req);

      boost::beast::flat_buffer buffer;
      http::response<http::string_body> res;
      http::read(stream, buffer, res);

      close_http_socket(stream.socket());
      if (res.result() == http::status::ok) {
        return true;
      }
    } catch (const std::exception& ex) {
      (void)ex;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  return false;
}

inline holder::platform::Db open_db_with_schema(const std::filesystem::path& db_path) {
  holder::platform::Db db;
  db.open(db_path);

  std::filesystem::path schema_path = SCHEMA_SQL_PATH;
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  return db;
}

inline void create_project(
    holder::platform::Db& db,
    const std::string& project_id,
    const std::string& root_path = "/tmp/project"
) {
  holder::project::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = root_path;
  project.privacy_mode = "plain";
  project.project_key_id.reset();
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

inline nlohmann::json http_json_request(
    const std::string& bind,
    unsigned short port,
    const std::string& token,
    boost::beast::http::verb method,
    const std::string& target,
    const nlohmann::json& body,
    boost::beast::http::status expected
) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bind, std::to_string(port));

  boost::beast::tcp_stream stream(ioc);
  stream.expires_after(std::chrono::seconds(2));
  stream.connect(endpoints);

  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, bind);
  req.set(http::field::user_agent, "holder-tests");
  req.keep_alive(false);
  if (!token.empty()) {
    req.set(http::field::authorization, "Bearer " + token);
  }
  if (!body.is_null() && !body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body.dump();
    req.prepare_payload();
  }

  http::write(stream, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  close_http_socket(stream.socket());

  REQUIRE(res.result() == expected);
  return nlohmann::json::parse(res.body());
}

struct HttpResult {
  boost::beast::http::status status;
  std::string body;
  std::string content_type;
};

class EnvGuard {
 public:
  EnvGuard(const char* key, const std::string& value)
      : key_(key) {
    const char* current = std::getenv(key_);
    if (current) {
      old_ = current;
    }
    set_env(value);
  }

  ~EnvGuard() {
    if (old_.has_value()) {
      set_env(old_.value());
    } else {
      unset_env();
    }
  }

 private:
  void set_env(const std::string& value) {
#ifdef _WIN32
    _putenv_s(key_, value.c_str());
#else
    setenv(key_, value.c_str(), 1);
#endif
  }

  void unset_env() {
#ifdef _WIN32
    _putenv_s(key_, "");
#else
    unsetenv(key_);
#endif
  }

  const char* key_;
  std::optional<std::string> old_;
};

inline void ensure_uuid_seeded() { static EnvGuard guard("HOLDER_UUID_SEED", "1337"); }

inline HttpResult http_request_raw(
    const std::string& bind,
    unsigned short port,
    const std::string& token,
    boost::beast::http::verb method,
    const std::string& target
) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bind, std::to_string(port));

  boost::beast::tcp_stream stream(ioc);
  stream.expires_after(std::chrono::seconds(2));
  stream.connect(endpoints);

  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, bind);
  req.set(http::field::user_agent, "holder-tests");
  req.keep_alive(false);
  if (!token.empty()) {
    req.set(http::field::authorization, "Bearer " + token);
  }

  http::write(stream, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(stream, buffer, res);

  close_http_socket(stream.socket());

  HttpResult result;
  result.status = res.result();
  result.body = res.body();
  const auto type_it = res.find(http::field::content_type);
  if (type_it != res.end()) {
    result.content_type = std::string(type_it->value());
  }
  return result;
}

} // namespace holder::test
