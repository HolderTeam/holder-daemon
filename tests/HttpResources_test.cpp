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

TEST_CASE("HTTP resources validation and route variants", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1", (dir / "project_repo").string());

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

  const auto missing_fields = http_json_request(bound.bind, bound.port, token,
                                                boost::beast::http::verb::post,
                                                "/resources",
                                                nlohmann::json{{"project_id", "proj-1"}},
                                                boost::beast::http::status::bad_request);
  REQUIRE(missing_fields["error"]["code"] == "bad_request");

  nlohmann::json create_body = {
      {"resource_id", "res-explicit"},
      {"project_id", "proj-1"},
      {"kind", "file"},
      {"uri", "file:///tmp/a.txt"},
      {"label", "A"},
      {"created_at", 111},
      {"updated_at", 222}
  };
  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/resources",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["resource_id"] == "res-explicit");

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/resources?project_id=proj-1",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["created_at"] == 111);
  REQUIRE(listed["data"][0]["updated_at"] == 222);

  const auto conflict = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::post,
                                          "/resources",
                                          create_body,
                                          boost::beast::http::status::conflict);
  REQUIRE(conflict["error"]["code"] == "conflict");

  const auto missing_updated_at = http_json_request(bound.bind, bound.port, token,
                                                    boost::beast::http::verb::patch,
                                                    "/resources/res-explicit",
                                                    nlohmann::json{{"label", "B"}},
                                                    boost::beast::http::status::bad_request);
  REQUIRE(missing_updated_at["error"]["code"] == "bad_request");

  const auto not_found_patch = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::patch,
                                                 "/resources/nope",
                                                 nlohmann::json{{"updated_at", 300}},
                                                 boost::beast::http::status::not_found);
  REQUIRE(not_found_patch["error"]["code"] == "not_found");

  const auto patch_ok = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::patch,
                                          "/resources/res-explicit",
                                          nlohmann::json{
                                              {"kind", "url"},
                                              {"uri", "https://example.com/new"},
                                              {"desc", "D"},
                                              {"updated_at", 333}},
                                          boost::beast::http::status::ok);
  REQUIRE(patch_ok["ok"] == true);

  const auto listed_after = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/resources?project_id=proj-1",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(listed_after["data"][0]["kind"] == "url");
  REQUIRE(listed_after["data"][0]["uri"] == "https://example.com/new");
  REQUIRE(listed_after["data"][0]["desc"] == "D");

  const auto route_not_found = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::patch,
                                                 "/resources/",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::not_found);
  REQUIRE(route_not_found["error"]["code"] == "not_found");

  const auto bad_method = http_json_request(bound.bind, bound.port, token,
                                            boost::beast::http::verb::get,
                                            "/resources/res-explicit",
                                            nlohmann::json::object(),
                                            boost::beast::http::status::not_found);
  REQUIRE(bad_method["error"]["code"] == "not_found");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP resources catch branches on repository failures", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1", (dir / "project_repo").string());

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

  db.exec("DROP TABLE resources;");

  const auto get_fail = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::get,
                                          "/resources?project_id=proj-1",
                                          nlohmann::json::object(),
                                          boost::beast::http::status::bad_request);
  REQUIRE(get_fail["error"]["code"] == "bad_request");

  const auto patch_fail = http_json_request(bound.bind, bound.port, token,
                                            boost::beast::http::verb::patch,
                                            "/resources/any",
                                            nlohmann::json{{"updated_at", 1}},
                                            boost::beast::http::status::bad_request);
  REQUIRE(patch_fail["error"]["code"] == "bad_request");

  const auto delete_fail = http_json_request(bound.bind, bound.port, token,
                                             boost::beast::http::verb::delete_,
                                             "/resources/any",
                                             nlohmann::json::object(),
                                             boost::beast::http::status::bad_request);
  REQUIRE(delete_fail["error"]["code"] == "bad_request");

  std::raise(SIGTERM);
  server_thread.join();
}
