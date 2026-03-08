#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::ensure_uuid_seeded;
using holder::test::EnvGuard;

TEST_CASE("HTTP first-run flow creates project and card", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  ensure_uuid_seeded();

  const auto projects_root = dir / "projects_root";
  std::filesystem::create_directories(projects_root);
  EnvGuard root_env("HOLDER_PROJECTS_ROOT", projects_root.string());

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

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

  const auto initial_projects = http_json_request(bound.bind, bound.port, token,
                                                  boost::beast::http::verb::get,
                                                  "/projects",
                                                  nlohmann::json::object(),
                                                  boost::beast::http::status::ok);
  REQUIRE(initial_projects["ok"] == true);
  REQUIRE(initial_projects["data"].is_array());

  nlohmann::json project_body = {
      {"name", "First Project"}
  };

  const auto created_project = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::post,
                                                 "/projects",
                                                 project_body,
                                                 boost::beast::http::status::created);
  REQUIRE(created_project["ok"] == true);
  REQUIRE(created_project["data"]["project_id"].is_string());
  const std::string project_id = created_project["data"]["project_id"].get<std::string>();

  nlohmann::json card_body = {
      {"project_id", project_id},
      {"title", "First Card"},
      {"content", "hello"}
  };

  const auto created_card = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::post,
                                              "/cards",
                                              card_body,
                                              boost::beast::http::status::created);
  REQUIRE(created_card["ok"] == true);
  REQUIRE(created_card["data"]["card_id"].is_string());
  const std::string card_id = created_card["data"]["card_id"].get<std::string>();

  const auto listed_cards = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/cards?project_id=" + project_id,
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(listed_cards["ok"] == true);
  REQUIRE(listed_cards["data"].is_array());
  REQUIRE(listed_cards["data"].size() == 1);
  REQUIRE(listed_cards["data"][0]["card_id"] == card_id);

  const auto fetched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/cards/" + card_id,
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(fetched["data"]["title"] == "First Card");
  REQUIRE(fetched["data"]["content"] == "hello");

  std::raise(SIGTERM);
  server_thread.join();
}
