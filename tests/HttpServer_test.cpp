#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/HttpServer.h"
#include "core/Signal.h"
#include "git/GitRepo.h"
#include "index/FtsIndexer.h"
#include "store/CardStore.h"
#include "model/Project.h"
#include "model/AiThread.h"
#include "model/AiMessage.h"
#include "store/AiMessageRepo.h"
#include "store/AiThreadRepo.h"
#include "store/Db.h"
#include "store/ProjectRepo.h"

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_http_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

nlohmann::json get_health(const std::string& bind,
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

holder::store::Db open_db_with_schema(const std::filesystem::path& db_path) {
  holder::store::Db db;
  db.open(db_path);

  std::filesystem::path schema_path = SCHEMA_SQL_PATH;
  std::ifstream in(schema_path);
  REQUIRE(in.is_open());
  std::string sql((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  db.exec(sql);

  return db;
}

void create_project(holder::store::Db& db, const std::string& project_id) {
  holder::store::ProjectRepo repo(db);
  holder::model::Project project;
  project.project_id = project_id;
  project.name = "Project";
  project.root_path = "/tmp/project";
  project.created_at = 1;
  project.updated_at = 1;
  repo.create(project);
}

nlohmann::json http_json_request(const std::string& bind,
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

} // namespace

TEST_CASE("HTTP /health returns ok with valid token", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto payload = get_health(bound.bind, bound.port, token);
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["db_ok"] == true);
  REQUIRE(payload["data"]["api_version"] == "0.1");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card create/get/patch", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, repo, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  nlohmann::json create_body = {
      {"card_id", "abcd1234"},
      {"project_id", "proj-1"},
      {"title", "First"},
      {"content", "hello"},
      {"created_at", 10},
      {"updated_at", 10}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/cards",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);

  const auto fetched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/cards/abcd1234",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(fetched["data"]["content"] == "hello");

  nlohmann::json patch_body = {
      {"content", "updated"},
      {"updated_at", 20},
      {"title", "Renamed"}
  };

  const auto patched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::patch,
                                         "/cards/abcd1234",
                                         patch_body,
                                         boost::beast::http::status::ok);
  REQUIRE(patched["ok"] == true);

  const auto fetched_again = http_json_request(bound.bind, bound.port, token,
                                               boost::beast::http::verb::get,
                                               "/cards/abcd1234",
                                               nlohmann::json::object(),
                                               boost::beast::http::status::ok);
  REQUIRE(fetched_again["data"]["title"] == "Renamed");
  REQUIRE(fetched_again["data"]["content"] == "updated");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card create rejects duplicate card_id", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, repo, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  nlohmann::json create_body = {
      {"card_id", "dup1234"},
      {"project_id", "proj-1"},
      {"title", "First"},
      {"content", "hello"},
      {"created_at", 10},
      {"updated_at", 10}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/cards",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);

  const auto conflict = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::post,
                                          "/cards",
                                          create_body,
                                          boost::beast::http::status::conflict);
  REQUIRE(conflict["ok"] == false);
  REQUIRE(conflict["error"]["code"] == "conflict");
  REQUIRE(conflict["error"]["message"].is_string());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card endpoints reject missing fields", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, repo, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  nlohmann::json bad_create = {
      {"card_id", "abcd1234"},
      {"project_id", "proj-1"},
      {"title", "First"}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/cards",
                                         bad_create,
                                         boost::beast::http::status::bad_request);
  REQUIRE(created["ok"] == false);
  REQUIRE(created["error"]["code"] == "bad_request");
  REQUIRE(created["error"]["message"].is_string());

  nlohmann::json bad_patch = {
      {"updated_at", 20}
  };

  const auto patched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::patch,
                                         "/cards/abcd1234",
                                         bad_patch,
                                         boost::beast::http::status::bad_request);
  REQUIRE(patched["ok"] == false);
  REQUIRE(patched["error"]["code"] == "bad_request");
  REQUIRE(patched["error"]["message"].is_string());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card endpoints reject invalid token", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, repo, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto unauthorized = http_json_request(bound.bind, bound.port, "badtoken",
                                              boost::beast::http::verb::get,
                                              "/cards/abcd1234",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::unauthorized);
  REQUIRE(unauthorized["ok"] == false);
  REQUIRE(unauthorized["error"]["code"] == "unauthorized");
  REQUIRE(unauthorized["error"]["message"].is_string());

  const auto missing = http_json_request(bound.bind, bound.port, "",
                                         boost::beast::http::verb::get,
                                         "/cards/abcd1234",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::unauthorized);
  REQUIRE(missing["ok"] == false);
  REQUIRE(missing["error"]["code"] == "unauthorized");
  REQUIRE(missing["error"]["message"].is_string());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card endpoints handle bad JSON and missing cards", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, repo, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto missing = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/cards/missing",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::not_found);
  REQUIRE(missing["ok"] == false);
  REQUIRE(missing["error"]["code"] == "not_found");
  REQUIRE(missing["error"]["message"].is_string());

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));
  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{http::verb::post, "/cards", 11};
  req.set(http::field::host, bound.bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);
  req.set(http::field::content_type, "application/json");
  req.body() = "{bad json";
  req.prepare_payload();

  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);
  socket.shutdown(tcp::socket::shutdown_both);

  REQUIRE(res.result() == http::status::bad_request);
  const auto parsed = nlohmann::json::parse(res.body());
  REQUIRE(parsed["ok"] == false);
  REQUIRE(parsed["error"]["code"] == "bad_request");
  REQUIRE(parsed["error"]["message"].is_string());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP search endpoints return results", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, repo, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  holder::model::Card card;
  card.card_id = "abcd9999";
  card.project_id = "proj-1";
  card.title = "Searchable";
  card.created_at = 10;
  card.updated_at = 10;
  card_store.create(card, "search term");

  const auto cards = http_json_request(bound.bind, bound.port, token,
                                       boost::beast::http::verb::get,
                                       "/search/cards?project_id=proj-1&q=search",
                                       nlohmann::json::object(),
                                       boost::beast::http::status::ok);
  REQUIRE(cards["ok"] == true);
  REQUIRE(cards["data"].is_array());
  REQUIRE(cards["data"].size() >= 1);
  REQUIRE(cards["data"][0]["card_id"].is_string());
  REQUIRE(cards["data"][0]["title"].is_string());
  REQUIRE(cards["data"][0]["updated_at"].is_number());
  REQUIRE(cards["data"][0]["created_at"].is_number());
  REQUIRE(cards["data"][0]["snippet"].is_string());
  REQUIRE(cards["data"][0]["rank"].is_number());

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 11;
  thread.updated_at = 11;
  holder::store::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

  holder::model::AiMessage msg;
  msg.message_id = "msg-1";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "search ai";
  msg.created_at = 12;
  holder::store::AiMessageRepo msg_repo(db, &fts);
  msg_repo.append(msg);

  const auto messages = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::get,
                                          "/search/ai?project_id=proj-1&q=search",
                                          nlohmann::json::object(),
                                          boost::beast::http::status::ok);
  REQUIRE(messages["ok"] == true);
  REQUIRE(messages["data"].is_array());
  REQUIRE(messages["data"].size() >= 1);
  REQUIRE(messages["data"][0]["message_id"].is_string());
  REQUIRE(messages["data"][0]["created_at"].is_number());
  REQUIRE(messages["data"][0]["snippet"].is_string());
  REQUIRE(messages["data"][0]["rank"].is_number());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP search endpoints reject missing params", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1");

  holder::git::GitRepo repo;
  const auto repo_dir = dir / "repo";
  repo.open_or_init(repo_dir);

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, repo, &fts);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto missing_project = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::get,
                                                 "/search/cards?q=term",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::bad_request);
  REQUIRE(missing_project["ok"] == false);
  REQUIRE(missing_project["error"]["code"] == "bad_request");
  REQUIRE(missing_project["error"]["message"].is_string());

  const auto missing_q = http_json_request(bound.bind, bound.port, token,
                                           boost::beast::http::verb::get,
                                           "/search/ai?project_id=proj-1",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::bad_request);
  REQUIRE(missing_q["ok"] == false);
  REQUIRE(missing_q["error"]["code"] == "bad_request");
  REQUIRE(missing_q["error"]["message"].is_string());

  std::raise(SIGTERM);
  server_thread.join();
}
