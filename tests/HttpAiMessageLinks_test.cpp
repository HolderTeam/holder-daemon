#include "http_test_helpers.h"

#include "model/AiThread.h"
#include "store/AiMessageRepo.h"
#include "store/AiThreadRepo.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai message links create/list/backlinks", "[http]") {
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
  holder::store::AiMessageRepo msg_repo(db, &fts);

  holder::model::AiMessage msg1;
  msg1.message_id = "msg-1";
  msg1.thread_id = "thread-1";
  msg1.role = "user";
  msg1.source = "manual";
  msg1.content = "Hello";
  msg1.created_at = 2;
  msg_repo.append(msg1);

  holder::model::AiMessage msg2 = msg1;
  msg2.message_id = "msg-2";
  msg2.content = "Hi";
  msg2.created_at = 3;
  msg_repo.append(msg2);

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

  nlohmann::json link_body = {
      {"to_card_id", "msg-2"},
      {"to_type", "ai_message"},
      {"kind", "ref"}
  };
  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/ai/messages/msg-1/links",
                                         link_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["to_type"] == "ai_message");

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/ai/messages/msg-1/links",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["to_card_id"] == "msg-2");
  REQUIRE(listed["data"][0]["to_type"] == "ai_message");

  const auto backlinks = http_json_request(bound.bind, bound.port, token,
                                           boost::beast::http::verb::get,
                                           "/ai/messages/msg-2/backlinks",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::ok);
  REQUIRE(backlinks["ok"] == true);
  REQUIRE(backlinks["data"].is_array());
  REQUIRE(backlinks["data"].size() == 1);
  REQUIRE(backlinks["data"][0]["from_card_id"] == "msg-1");

  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::delete_,
                    "/ai/messages/msg-2",
                    nlohmann::json::object(),
                    boost::beast::http::status::ok);

  const auto hidden = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/ai/messages/msg-1/links",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(hidden["data"].size() == 0);

  const auto visible = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/ai/messages/msg-1/links?include_deleted=1",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(visible["data"].size() == 1);

  nlohmann::json invalid_body = {
      {"to_card_id", "msg-2"},
      {"to_type", "unknown"},
      {"kind", "ref"}
  };
  const auto invalid = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/ai/messages/msg-1/links",
                                         invalid_body,
                                         boost::beast::http::status::bad_request);
  REQUIRE(invalid["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}
