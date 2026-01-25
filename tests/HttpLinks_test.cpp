#include "http_test_helpers.h"

using holder::test::create_project;
using holder::test::ensure_uuid_seeded;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP card links create/list/delete", "[http]") {
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

  nlohmann::json card_a = {
      {"card_id", "card-a"},
      {"project_id", "proj-1"},
      {"title", "Card A"},
      {"content", "alpha"},
      {"created_at", 10},
      {"updated_at", 10}
  };
  nlohmann::json card_b = {
      {"card_id", "card-b"},
      {"project_id", "proj-1"},
      {"title", "Card B"},
      {"content", "beta"},
      {"created_at", 11},
      {"updated_at", 11}
  };

  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/cards",
                    card_a,
                    boost::beast::http::status::created);
  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/cards",
                    card_b,
                    boost::beast::http::status::created);

  nlohmann::json link_body = {
      {"to_card_id", "card-b"},
      {"to_type", "card"},
      {"kind", "ref"},
      {"label", "See"},
      {"created_at", 123}
  };
  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/cards/card-a/links",
                                         link_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["to_card_id"] == "card-b");
  REQUIRE(created["data"]["to_type"] == "card");
  REQUIRE(created["data"]["kind"] == "ref");
  REQUIRE(created["data"]["label"] == "See");

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/cards/card-a/links",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["to_card_id"] == "card-b");
  REQUIRE(listed["data"][0]["to_type"] == "card");
  REQUIRE(listed["data"][0]["kind"] == "ref");
  REQUIRE(listed["data"][0]["label"] == "See");

  const auto backlinks = http_json_request(bound.bind, bound.port, token,
                                           boost::beast::http::verb::get,
                                           "/cards/card-b/backlinks",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::ok);
  REQUIRE(backlinks["ok"] == true);
  REQUIRE(backlinks["data"].is_array());
  REQUIRE(backlinks["data"].size() == 1);
  REQUIRE(backlinks["data"][0]["from_card_id"] == "card-a");

  nlohmann::json delete_body = {
      {"to_card_id", "card-b"},
      {"kind", "ref"}
  };
  const auto deleted = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::delete_,
                                         "/cards/card-a/links",
                                         delete_body,
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  const auto listed_after = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/cards/card-a/links",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(listed_after["data"].size() == 0);

  std::raise(SIGTERM);
  server_thread.join();
}
