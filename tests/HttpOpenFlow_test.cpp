#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::ensure_uuid_seeded;

TEST_CASE("HTTP open flow lists projects then cards", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  ensure_uuid_seeded();

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

  nlohmann::json project_body = {
      {"name", "Project A"},
      {"root_path", "/tmp/project_a"}
  };

  const auto created_project = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::post,
                                                 "/projects",
                                                 project_body,
                                                 boost::beast::http::status::created);
  const std::string project_id = created_project["data"]["project_id"].get<std::string>();

  nlohmann::json card_body = {
      {"project_id", project_id},
      {"title", "Card A"},
      {"content", "hello"}
  };

  const auto created_card = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::post,
                                              "/cards",
                                              card_body,
                                              boost::beast::http::status::created);
  const std::string card_id = created_card["data"]["card_id"].get<std::string>();

  const auto projects = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::get,
                                          "/projects",
                                          nlohmann::json::object(),
                                          boost::beast::http::status::ok);
  REQUIRE(projects["ok"] == true);
  REQUIRE(projects["data"].is_array());

  bool found_project = false;
  for (const auto& item : projects["data"]) {
    if (item["project_id"] == project_id) {
      found_project = true;
      break;
    }
  }
  REQUIRE(found_project);

  const auto cards = http_json_request(bound.bind, bound.port, token,
                                       boost::beast::http::verb::get,
                                       "/cards?project_id=" + project_id,
                                       nlohmann::json::object(),
                                       boost::beast::http::status::ok);
  REQUIRE(cards["ok"] == true);
  REQUIRE(cards["data"].is_array());
  REQUIRE(cards["data"].size() == 1);
  REQUIRE(cards["data"][0]["card_id"] == card_id);

  std::raise(SIGTERM);
  server_thread.join();
}
