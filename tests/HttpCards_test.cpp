#include "http_test_helpers.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::ensure_uuid_seeded;

TEST_CASE("HTTP card create/get/patch", "[http]") {
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

  nlohmann::json create_body = {
      {"card_id", "abcd1234"},
      {"project_id", "proj-1"},
      {"title", "First"},
      {"content", "hello"},
      {"created_at", 10},
      {"updated_at", 10}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/cards",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);

  nlohmann::json auto_body = {
      {"project_id", "proj-1"},
      {"title", "Auto Card"},
      {"content", "auto"}
  };

  const auto created_auto = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::post,
                                              "/cards",
                                              auto_body,
                                              boost::beast::http::status::created);
  REQUIRE(created_auto["ok"] == true);
  REQUIRE(created_auto["data"]["card_id"].is_string());
  REQUIRE(created_auto["data"]["card_id"].get<std::string>().size() > 0);
  const std::string auto_id = created_auto["data"]["card_id"].get<std::string>();

  const auto fetched_auto = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/cards/" + auto_id,
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(fetched_auto["data"]["created_at"].get<long long>() > 0);
  REQUIRE(fetched_auto["data"]["updated_at"].get<long long>() > 0);

  const auto fetched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/cards/abcd1234",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(fetched["data"]["title"] == "First");
  REQUIRE(fetched["data"]["content"] == "hello");

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/cards?project_id=proj-1",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() >= 2);
  bool found_first = false;
  bool found_auto = false;
  for (const auto& item : listed["data"]) {
    if (item["card_id"] == "abcd1234") {
      found_first = true;
    }
    if (item["card_id"] == auto_id) {
      found_auto = true;
    }
  }
  REQUIRE(found_first);
  REQUIRE(found_auto);

  nlohmann::json update_body = {
      {"title", "First Updated"},
      {"content", "hello world"},
      {"updated_at", 20}
  };

  const auto updated = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::patch,
                                         "/cards/abcd1234",
                                         update_body,
                                         boost::beast::http::status::ok);
  REQUIRE(updated["ok"] == true);

  const auto fetched_after = http_json_request(bound.bind, bound.port, token,
                                               boost::beast::http::verb::get,
                                               "/cards/abcd1234",
                                               nlohmann::json::object(),
                                               boost::beast::http::status::ok);
  REQUIRE(fetched_after["data"]["title"] == "First Updated");
  REQUIRE(fetched_after["data"]["content"] == "hello world");

  const auto deleted = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::delete_,
                                         "/cards/abcd1234",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  const auto listed_deleted = http_json_request(bound.bind, bound.port, token,
                                                boost::beast::http::verb::get,
                                                "/cards?project_id=proj-1&include_deleted=1",
                                                nlohmann::json::object(),
                                                boost::beast::http::status::ok);
  bool found_deleted = false;
  for (const auto& item : listed_deleted["data"]) {
    if (item["card_id"] == "abcd1234") {
      found_deleted = true;
      REQUIRE(item["deleted_at"].is_number());
    }
  }
  REQUIRE(found_deleted);

  const auto restored = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::post,
                                          "/cards/abcd1234/restore",
                                          nlohmann::json::object(),
                                          boost::beast::http::status::ok);
  REQUIRE(restored["ok"] == true);

  const auto fetched_restored = http_json_request(bound.bind, bound.port, token,
                                                  boost::beast::http::verb::get,
                                                  "/cards/abcd1234",
                                                  nlohmann::json::object(),
                                                  boost::beast::http::status::ok);
  REQUIRE(fetched_restored["data"]["title"] == "First Updated");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card create rejects duplicate card_id", "[http]") {
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
      {"card_id", "abcd1234"},
      {"project_id", "proj-1"},
      {"title", "First"},
      {"content", "hello"},
      {"created_at", 10},
      {"updated_at", 10}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/cards",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);

  const auto conflict = http_json_request(bound.bind, bound.port, token,
                                          boost::beast::http::verb::post,
                                          "/cards",
                                          create_body,
                                          boost::beast::http::status::conflict);
  REQUIRE(conflict["ok"] == false);
  REQUIRE(conflict["error"]["code"] == "conflict");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card endpoints reject missing fields", "[http]") {
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

  const auto bad_create = http_json_request(bound.bind, bound.port, token,
                                            boost::beast::http::verb::post,
                                            "/cards",
                                            nlohmann::json::object(),
                                            boost::beast::http::status::bad_request);
  REQUIRE(bad_create["ok"] == false);
  REQUIRE(bad_create["error"]["code"] == "bad_request");

  const auto bad_patch = http_json_request(bound.bind, bound.port, token,
                                           boost::beast::http::verb::patch,
                                           "/cards/abcd1234",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::bad_request);
  REQUIRE(bad_patch["ok"] == false);
  REQUIRE(bad_patch["error"]["code"] == "bad_request");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card endpoints reject invalid token", "[http]") {
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

  const auto bad = http_json_request(bound.bind, bound.port, "badtoken",
                                     boost::beast::http::verb::get,
                                     "/cards/abcd1234",
                                     nlohmann::json::object(),
                                     boost::beast::http::status::unauthorized);
  REQUIRE(bad["ok"] == false);
  REQUIRE(bad["error"]["code"] == "unauthorized");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card endpoints handle bad JSON and missing cards", "[http]") {
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

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));

  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{http::verb::post, "/cards", 11};
  req.set(http::field::host, bound.bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);
  req.set(http::field::content_type, "application/json");
  req.body() = "{ invalid json";
  req.prepare_payload();

  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);

  socket.shutdown(tcp::socket::shutdown_both);

  REQUIRE(res.result() == http::status::bad_request);
  const auto error = nlohmann::json::parse(res.body());
  REQUIRE(error["ok"] == false);
  REQUIRE(error["error"]["code"] == "bad_request");
  REQUIRE(error["error"]["message"].is_string());

  const auto missing = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/cards/missing",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::not_found);
  REQUIRE(missing["ok"] == false);
  REQUIRE(missing["error"]["code"] == "not_found");

  std::raise(SIGTERM);
  server_thread.join();
}
