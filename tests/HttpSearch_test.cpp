#include "http_test_helpers.h"

#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "store/AiMessageRepo.h"
#include "store/AiThreadRepo.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP search endpoints return results", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, &fts);

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
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, &fts);

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
