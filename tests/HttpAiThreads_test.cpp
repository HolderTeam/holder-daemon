#include "http_test_helpers.h"

#include "model/Card.h"
#include "card/CardRepo.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai threads create/list/get/patch", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  holder::index::FtsIndexer fts(db);
  holder::store::CardStore card_store(db, &fts);
  holder::store::CardRepo card_repo(db);

  holder::model::Card card;
  card.card_id = "card-1";
  card.project_id = "proj-1";
  card.title = "Linked";
  card.rel_path = "cards/c1.md";
  card.sort_key = 0.0;
  card.created_at = 5;
  card.updated_at = 5;
  card_repo.create(card);

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
      {"project_id", "proj-1"},
      {"title", "Thread"},
      {"created_at", 10},
      {"updated_at", 10}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/ai/threads",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["thread_id"] == "thread-1");

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/ai/threads?project_id=proj-1",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() >= 1);

  const auto fetched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/ai/threads/thread-1",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(fetched["ok"] == true);
  REQUIRE(fetched["data"]["thread_id"] == "thread-1");
  REQUIRE(fetched["data"]["project_id"] == "proj-1");

  nlohmann::json patch_body = {
      {"title", "Updated"},
      {"card_id", "card-1"},
      {"updated_at", 20}
  };
  const auto patched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::patch,
                                         "/ai/threads/thread-1",
                                         patch_body,
                                         boost::beast::http::status::ok);
  REQUIRE(patched["ok"] == true);

  const auto fetched_after = http_json_request(bound.bind, bound.port, token,
                                               boost::beast::http::verb::get,
                                               "/ai/threads/thread-1",
                                               nlohmann::json::object(),
                                               boost::beast::http::status::ok);
  REQUIRE(fetched_after["data"]["title"] == "Updated");
  REQUIRE(fetched_after["data"]["card_id"] == "card-1");

  const auto missing_project = http_json_request(bound.bind, bound.port, token,
                                                boost::beast::http::verb::get,
                                                "/ai/threads",
                                                nlohmann::json::object(),
                                                boost::beast::http::status::bad_request);
  REQUIRE(missing_project["ok"] == false);

  const auto deleted = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::delete_,
                                         "/ai/threads/thread-1",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  std::raise(SIGTERM);
  server_thread.join();
}
