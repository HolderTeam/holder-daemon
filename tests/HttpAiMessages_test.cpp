#include "http_test_helpers.h"

#include "model/AiThread.h"
#include "ai/AiThreadRepo.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai messages create/list/get", "[http]") {
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

  nlohmann::json create_body = {
      {"thread_id", "thread-1"},
      {"role", "user"},
      {"source", "manual"},
      {"content", "Hello"},
      {"created_at", 10}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/ai/messages",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["message_id"].is_string());
  const std::string msg_id = created["data"]["message_id"].get<std::string>();

  const auto fetched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/ai/messages/" + msg_id,
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(fetched["ok"] == true);
  REQUIRE(fetched["data"]["message_id"] == msg_id);
  REQUIRE(fetched["data"]["thread_id"] == "thread-1");
  REQUIRE(fetched["data"]["content"] == "Hello");

  nlohmann::json patch_body = {
      {"content", "Updated"},
      {"provider", "Ollama"},
      {"created_at", 20}
  };
  const auto patched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::patch,
                                         "/ai/messages/" + msg_id,
                                         patch_body,
                                         boost::beast::http::status::ok);
  REQUIRE(patched["ok"] == true);

  const auto fetched_after = http_json_request(bound.bind, bound.port, token,
                                               boost::beast::http::verb::get,
                                               "/ai/messages/" + msg_id,
                                               nlohmann::json::object(),
                                               boost::beast::http::status::ok);
  REQUIRE(fetched_after["data"]["content"] == "Updated");
  REQUIRE(fetched_after["data"]["provider"] == "Ollama");

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/ai/messages?thread_id=thread-1",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() >= 1);

  const auto missing_thread = http_json_request(bound.bind, bound.port, token,
                                                boost::beast::http::verb::get,
                                                "/ai/messages",
                                                nlohmann::json::object(),
                                                boost::beast::http::status::bad_request);
  REQUIRE(missing_thread["ok"] == false);

  const auto deleted = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::delete_,
                                         "/ai/messages/" + msg_id,
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  const auto fetched_deleted = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::get,
                                                 "/ai/messages/" + msg_id,
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::not_found);
  REQUIRE(fetched_deleted["ok"] == false);

  const auto restored = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::post,
                                          "/ai/messages/" + msg_id + "/restore",
                                          nlohmann::json::object(),
                                          boost::beast::http::status::ok);
  REQUIRE(restored["ok"] == true);

  std::raise(SIGTERM);
  server_thread.join();
}
