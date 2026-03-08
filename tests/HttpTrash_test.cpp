#include "http_test_helpers.h"

#include "model/AiThread.h"
#include "ai/AiThreadRepo.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP trash list/empty/hard delete", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::store::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

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

  nlohmann::json card_body = {
      {"card_id", "card-1"},
      {"project_id", "proj-1"},
      {"title", "Card"},
      {"content", "Hello"},
      {"created_at", 10},
      {"updated_at", 10}
  };
  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/cards",
                    card_body,
                    boost::beast::http::status::created);

  nlohmann::json msg_body = {
      {"message_id", "msg-1"},
      {"thread_id", "thread-1"},
      {"role", "user"},
      {"source", "manual"},
      {"content", "Hi"},
      {"created_at", 11}
  };
  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/ai/messages",
                    msg_body,
                    boost::beast::http::status::created);

  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::delete_,
                    "/cards/card-1",
                    nlohmann::json::object(),
                    boost::beast::http::status::ok);
  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::delete_,
                    "/ai/messages/msg-1",
                    nlohmann::json::object(),
                    boost::beast::http::status::ok);

  const auto trash_list = http_json_request(bound.bind, bound.port, token,
                                            boost::beast::http::verb::get,
                                            "/trash?project_id=proj-1",
                                            nlohmann::json::object(),
                                            boost::beast::http::status::ok);
  REQUIRE(trash_list["ok"] == true);
  REQUIRE(trash_list["data"].is_array());
  REQUIRE(trash_list["data"].size() == 2);

  const auto trash_cards = http_json_request(bound.bind, bound.port, token,
                                             boost::beast::http::verb::get,
                                             "/trash?project_id=proj-1&type=card",
                                             nlohmann::json::object(),
                                             boost::beast::http::status::ok);
  REQUIRE(trash_cards["ok"] == true);
  REQUIRE(trash_cards["data"].is_array());
  REQUIRE(trash_cards["data"].size() == 1);
  REQUIRE(trash_cards["data"][0]["type"] == "card");

  const auto trash_msgs = http_json_request(bound.bind, bound.port, token,
                                            boost::beast::http::verb::get,
                                            "/trash?project_id=proj-1&type=ai_message",
                                            nlohmann::json::object(),
                                            boost::beast::http::status::ok);
  REQUIRE(trash_msgs["ok"] == true);
  REQUIRE(trash_msgs["data"].is_array());
  REQUIRE(trash_msgs["data"].size() == 1);
  REQUIRE(trash_msgs["data"][0]["type"] == "ai_message");

  const auto hard_deleted = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::delete_,
                                              "/trash/card/card-1",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(hard_deleted["ok"] == true);

  const auto emptied = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::delete_,
                                         "/trash?project_id=proj-1",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(emptied["ok"] == true);

  std::raise(SIGTERM);
  server_thread.join();
}
