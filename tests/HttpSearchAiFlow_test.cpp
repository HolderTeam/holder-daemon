#include "http_test_helpers.h"

#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::ensure_uuid_seeded;

TEST_CASE("HTTP search AI flow finds message", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());
  ensure_uuid_seeded();

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
  msg.content = "search ai flow";
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

  bool found = false;
  for (const auto& item : messages["data"]) {
    if (item["message_id"] == "msg-1") {
      found = true;
      break;
    }
  }
  REQUIRE(found);

  std::raise(SIGTERM);
  server_thread.join();
}
