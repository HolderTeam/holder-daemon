#pragma once

#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/HttpServer.h"
#include "platform/Signal.h"
#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "model/Project.h"
#include "card/CardStore.h"
#include "platform/Db.h"
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
#include <thread>

namespace holder::test {

inline std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_http_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

inline nlohmann::json get_health(const std::string& bind,
                                 unsigned short port,
                                 const std::string& token) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bind, std::to_string(port));

  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{http::verb::get, "/health", 11};
  req.set(http::field::host, bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);

  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);

  socket.shutdown(tcp::socket::shutdown_both);

  REQUIRE(res.result() == http::status::ok);
  return nlohmann::json::parse(res.body());
}

inline holder::store::Db open_db_with_schema(const std::filesystem::path& db_path) {
  holder::store::Db db;
  db.open(db_path);

  std::filesystem::path schema_path = SCHEMA_SQL_PATH;
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  return db;
}

inline void create_project(holder::store::Db& db,
                           const std::string& project_id,
                           const std::string& root_path = "/tmp/project") {
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

inline nlohmann::json http_json_request(const std::string& bind,
                                        unsigned short port,
                                        const std::string& token,
                                        boost::beast::http::verb method,
                                        const std::string& target,
                                        const nlohmann::json& body,
                                        boost::beast::http::status expected) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bind, std::to_string(port));

  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, bind);
  req.set(http::field::user_agent, "holder-tests");
  if (!token.empty()) {
    req.set(http::field::authorization, "Bearer " + token);
  }
  if (!body.is_null() && !body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body.dump();
    req.prepare_payload();
  }

  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);

  socket.shutdown(tcp::socket::shutdown_both);

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
  EnvGuard(const char* key, std::string value) : key_(key) {
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

inline void ensure_uuid_seeded() {
  static EnvGuard guard("HOLDER_UUID_SEED", "1337");
}

inline HttpResult http_request_raw(const std::string& bind,
                                   unsigned short port,
                                   const std::string& token,
                                   boost::beast::http::verb method,
                                   const std::string& target) {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bind, std::to_string(port));

  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, bind);
  req.set(http::field::user_agent, "holder-tests");
  if (!token.empty()) {
    req.set(http::field::authorization, "Bearer " + token);
  }

  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);

  socket.shutdown(tcp::socket::shutdown_both);

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
