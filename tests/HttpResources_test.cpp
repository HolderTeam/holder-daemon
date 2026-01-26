#include "http_test_helpers.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP resources create/list/delete", "[http]") {
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

  nlohmann::json create_body = {
      {"project_id", "proj-1"},
      {"kind", "url"},
      {"uri", "https://example.com"},
      {"label", "Example"},
      {"desc", "Reference"}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/resources",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["resource_id"].is_string());
  const std::string resource_id = created["data"]["resource_id"].get<std::string>();

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/resources?project_id=proj-1",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["resource_id"] == resource_id);
  REQUIRE(listed["data"][0]["kind"] == "url");
  REQUIRE(listed["data"][0]["uri"] == "https://example.com");

  nlohmann::json patch_body = {
      {"label", "Updated"},
      {"desc", nullptr},
      {"updated_at", 20}
  };
  const auto patched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::patch,
                                         "/resources/" + resource_id,
                                         patch_body,
                                         boost::beast::http::status::ok);
  REQUIRE(patched["ok"] == true);

  const auto listed_after_patch = http_json_request(bound.bind, bound.port, token,
                                                    boost::beast::http::verb::get,
                                                    "/resources?project_id=proj-1",
                                                    nlohmann::json::object(),
                                                    boost::beast::http::status::ok);
  REQUIRE(listed_after_patch["data"][0]["label"] == "Updated");
  REQUIRE(listed_after_patch["data"][0]["desc"].is_null());

  const auto deleted = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::delete_,
                                         "/resources/" + resource_id,
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  const auto listed_after = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/resources?project_id=proj-1",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(listed_after["data"].size() == 0);

  const auto missing_project = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::get,
                                                 "/resources",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::bad_request);
  REQUIRE(missing_project["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}
