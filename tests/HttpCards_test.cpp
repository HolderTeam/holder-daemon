#include "http_test_helpers.h"

#include <fstream>
#include <optional>

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

  const auto children_empty = http_json_request(bound.bind, bound.port, token,
                                                boost::beast::http::verb::get,
                                                "/cards?project_id=proj-1&view=tree&parent_card_id=abcd1234",
                                                nlohmann::json::object(),
                                                boost::beast::http::status::ok);
  REQUIRE(children_empty["ok"] == true);
  REQUIRE(children_empty["data"].is_array());
  REQUIRE(children_empty["data"].empty());

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

  nlohmann::json move_body = {
      {"parent_card_id", auto_id},
      {"sort_key", 777.0},
      {"updated_at", 30}
  };

  const auto moved = http_json_request(bound.bind, bound.port, token,
                                       boost::beast::http::verb::patch,
                                       "/cards/abcd1234",
                                       move_body,
                                       boost::beast::http::status::ok);
  REQUIRE(moved["ok"] == true);

  const auto fetched_moved = http_json_request(bound.bind, bound.port, token,
                                               boost::beast::http::verb::get,
                                               "/cards/abcd1234",
                                               nlohmann::json::object(),
                                               boost::beast::http::status::ok);
  REQUIRE(fetched_moved["data"]["parent_card_id"] == auto_id);
  REQUIRE(fetched_moved["data"]["sort_key"].get<double>() == 777.0);
  REQUIRE(fetched_moved["data"]["updated_at"] == 30);

  const auto listed_all = http_json_request(bound.bind, bound.port, token,
                                            boost::beast::http::verb::get,
                                            "/cards?project_id=proj-1&view=recent",
                                            nlohmann::json::object(),
                                            boost::beast::http::status::ok);
  bool found_nested_in_all = false;
  for (const auto& item : listed_all["data"]) {
    if (item["card_id"] == "abcd1234") {
      found_nested_in_all = true;
      REQUIRE(item["parent_card_id"] == auto_id);
    }
  }
  REQUIRE(found_nested_in_all);

  const auto listed_root = http_json_request(bound.bind, bound.port, token,
                                             boost::beast::http::verb::get,
                                             "/cards?project_id=proj-1&view=tree",
                                             nlohmann::json::object(),
                                             boost::beast::http::status::ok);
  bool found_nested_in_root = false;
  for (const auto& item : listed_root["data"]) {
    if (item["card_id"] == "abcd1234") {
      found_nested_in_root = true;
    }
  }
  REQUIRE(!found_nested_in_root);

  nlohmann::json reparent_root_body = {
      {"parent_card_id", nullptr},
      {"updated_at", 31}
  };

  const auto reparented = http_json_request(bound.bind, bound.port, token,
                                            boost::beast::http::verb::patch,
                                            "/cards/abcd1234",
                                            reparent_root_body,
                                            boost::beast::http::status::ok);
  REQUIRE(reparented["ok"] == true);

  const auto fetched_reparented = http_json_request(bound.bind, bound.port, token,
                                                    boost::beast::http::verb::get,
                                                    "/cards/abcd1234",
                                                    nlohmann::json::object(),
                                                    boost::beast::http::status::ok);
  REQUIRE(fetched_reparented["data"]["parent_card_id"].is_null());
  REQUIRE(fetched_reparented["data"]["updated_at"] == 31);

  const auto invalid_view = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/cards?project_id=proj-1&view=bogus",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::bad_request);
  REQUIRE(invalid_view["ok"] == false);

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

TEST_CASE("HTTP card content stays plaintext over API for encrypted project", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
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

  const auto project_root = dir / "enc_project_repo";
  const auto created_project = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects",
      {
          {"project_id", "proj-enc"},
          {"name", "Encrypted Project"},
          {"root_path", project_root.string()},
      },
      boost::beast::http::status::created);
  REQUIRE(created_project["ok"] == true);
  REQUIRE(created_project["data"]["privacy_mode"] == "encrypted_git");
  REQUIRE(created_project["data"]["project_key_id"].is_string());

  const auto created_card = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards",
      {
          {"card_id", "efgh1234"},
          {"project_id", "proj-enc"},
          {"title", "Encrypted Card"},
          {"content", "this is secret plaintext"},
          {"created_at", 10},
          {"updated_at", 10},
      },
      boost::beast::http::status::created);
  REQUIRE(created_card["ok"] == true);

  const auto fetched = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::get,
                                         "/cards/efgh1234",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(fetched["data"]["content"] == "this is secret plaintext");

  const auto rel_path = std::string(created_card["data"]["rel_path"]);
  const auto full_path = project_root / rel_path;
  std::ifstream in(full_path, std::ios::binary);
  REQUIRE(in.is_open());
  const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  REQUIRE(raw.rfind("HolderPriv1\n", 0) == 0);
  REQUIRE(raw.find("this is secret plaintext") == std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card move intent endpoint", "[http]") {
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

  auto create_card = [&](const std::string& id, const std::string& title, int t) {
    return http_json_request(bound.bind,
                             bound.port,
                             token,
                             boost::beast::http::verb::post,
                             "/cards",
                             {
                                 {"card_id", id},
                                 {"project_id", "proj-1"},
                                 {"title", title},
                                 {"content", title},
                                 {"created_at", t},
                                 {"updated_at", t},
                             },
                             boost::beast::http::status::created);
  };

  REQUIRE(create_card("11111111-1111-4111-8111-111111111111", "A", 10)["ok"] == true);
  REQUIRE(create_card("22222222-2222-4222-8222-222222222222", "B", 11)["ok"] == true);
  REQUIRE(create_card("33333333-3333-4333-8333-333333333333", "C", 12)["ok"] == true);
  REQUIRE(create_card("44444444-4444-4444-8444-444444444444", "D", 13)["ok"] == true);

  const auto moved_into = http_json_request(bound.bind,
                                            bound.port,
                                            token,
                                            boost::beast::http::verb::post,
                                            "/cards/11111111-1111-4111-8111-111111111111/move",
                                            {{"project_id", "proj-1"},
                                             {"intent", "into"},
                                             {"target_card_id", "22222222-2222-4222-8222-222222222222"}},
                                            boost::beast::http::status::ok);
  REQUIRE(moved_into["ok"] == true);
  REQUIRE(moved_into["data"]["parent_card_id"] == "22222222-2222-4222-8222-222222222222");
  REQUIRE(moved_into["data"]["moved_into_title"] == "B");

  const auto cycle = http_json_request(bound.bind,
                                       bound.port,
                                       token,
                                       boost::beast::http::verb::post,
                                       "/cards/22222222-2222-4222-8222-222222222222/move",
                                       {{"project_id", "proj-1"},
                                        {"intent", "into"},
                                        {"target_card_id", "11111111-1111-4111-8111-111111111111"}},
                                       boost::beast::http::status::unprocessable_entity);
  REQUIRE(cycle["ok"] == false);
  REQUIRE(cycle["error"]["code"] == "move_would_create_cycle");

  const auto moved_before = http_json_request(bound.bind,
                                              bound.port,
                                              token,
                                              boost::beast::http::verb::post,
                                              "/cards/44444444-4444-4444-8444-444444444444/move",
                                              {{"project_id", "proj-1"},
                                               {"intent", "before"},
                                               {"target_card_id", "33333333-3333-4333-8333-333333333333"}},
                                              boost::beast::http::status::ok);
  REQUIRE(moved_before["ok"] == true);
  REQUIRE(moved_before["data"]["parent_card_id"].is_null());

  const auto all = http_json_request(bound.bind,
                                     bound.port,
                                     token,
                                     boost::beast::http::verb::get,
                                     "/cards?project_id=proj-1&view=recent",
                                     nlohmann::json::object(),
                                     boost::beast::http::status::ok);
  double c_sort = 0.0;
  double d_sort = 0.0;
  bool found_c = false;
  bool found_d = false;
  for (const auto& item : all["data"]) {
    if (item["card_id"] == "33333333-3333-4333-8333-333333333333") {
      found_c = true;
      c_sort = item["sort_key"].get<double>();
    }
    if (item["card_id"] == "44444444-4444-4444-8444-444444444444") {
      found_d = true;
      d_sort = item["sort_key"].get<double>();
    }
  }
  REQUIRE(found_c);
  REQUIRE(found_d);
  REQUIRE(d_sort < c_sort);

  const auto move_up = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::post,
                                         "/cards/11111111-1111-4111-8111-111111111111/move",
                                         {{"project_id", "proj-1"}, {"intent", "up_level"}},
                                         boost::beast::http::status::ok);
  REQUIRE(move_up["ok"] == true);
  REQUIRE(move_up["data"]["parent_card_id"].is_null());

  auto position_in_root_tree = [&](const std::string& id) -> int {
    const auto tree = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::get,
                                        "/cards?project_id=proj-1&view=tree",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
    REQUIRE(tree["ok"] == true);
    for (size_t i = 0; i < tree["data"].size(); ++i) {
      if (tree["data"][i]["card_id"] == id) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  const auto to_end_inferred_parent = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/33333333-3333-4333-8333-333333333333/move",
      {{"project_id", "proj-1"}, {"intent", "to_end"}},
      boost::beast::http::status::ok);
  REQUIRE(to_end_inferred_parent["ok"] == true);
  const int c_after_end = position_in_root_tree("33333333-3333-4333-8333-333333333333");
  REQUIRE(c_after_end >= 0);

  const auto to_start_inferred_parent = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/33333333-3333-4333-8333-333333333333/move",
      {{"project_id", "proj-1"}, {"intent", "to_start"}},
      boost::beast::http::status::ok);
  REQUIRE(to_start_inferred_parent["ok"] == true);
  const int c_after_start = position_in_root_tree("33333333-3333-4333-8333-333333333333");
  REQUIRE(c_after_start == 0);

  const auto left_noop = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::post,
                                           "/cards/33333333-3333-4333-8333-333333333333/move",
                                           {{"project_id", "proj-1"}, {"intent", "left"}},
                                           boost::beast::http::status::ok);
  REQUIRE(left_noop["ok"] == true);
  REQUIRE(position_in_root_tree("33333333-3333-4333-8333-333333333333") == 0);

  const auto right_move = http_json_request(bound.bind,
                                            bound.port,
                                            token,
                                            boost::beast::http::verb::post,
                                            "/cards/33333333-3333-4333-8333-333333333333/move",
                                            {{"project_id", "proj-1"}, {"intent", "right"}},
                                            boost::beast::http::status::ok);
  REQUIRE(right_move["ok"] == true);
  REQUIRE(position_in_root_tree("33333333-3333-4333-8333-333333333333") > 0);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card context endpoint", "[http]") {
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

  auto create_card = [&](const std::string& id,
                         const std::string& title,
                         int t,
                         const std::optional<std::string>& parent = std::nullopt) {
    nlohmann::json body = {
        {"card_id", id},
        {"project_id", "proj-1"},
        {"title", title},
        {"content", title},
        {"created_at", t},
        {"updated_at", t},
    };
    if (parent.has_value()) {
      body["parent_card_id"] = parent.value();
    }
    return http_json_request(bound.bind,
                             bound.port,
                             token,
                             boost::beast::http::verb::post,
                             "/cards",
                             body,
                             boost::beast::http::status::created);
  };

  REQUIRE(create_card("11111111-1111-4111-8111-111111111111", "A", 10)["ok"] == true);
  REQUIRE(create_card("22222222-2222-4222-8222-222222222222", "B", 11)["ok"] == true);
  REQUIRE(create_card("33333333-3333-4333-8333-333333333333",
                      "C",
                      12,
                      "11111111-1111-4111-8111-111111111111")["ok"] == true);
  REQUIRE(create_card("44444444-4444-4444-8444-444444444444",
                      "D",
                      13,
                      "33333333-3333-4333-8333-333333333333")["ok"] == true);

  const auto root_ctx = http_json_request(bound.bind,
                                          bound.port,
                                          token,
                                          boost::beast::http::verb::get,
                                          "/cards/context?project_id=proj-1&count=true",
                                          nlohmann::json::object(),
                                          boost::beast::http::status::ok);
  REQUIRE(root_ctx["ok"] == true);
  REQUIRE(root_ctx["data"]["project"]["project_id"] == "proj-1");
  REQUIRE(root_ctx["data"]["project"]["name"] == "Project");
  REQUIRE(root_ctx["data"]["current_parent_card_id"].is_null());
  REQUIRE(root_ctx["data"]["breadcrumbs"].is_array());
  REQUIRE(root_ctx["data"]["breadcrumbs"].size() == 1);
  REQUIRE(root_ctx["data"]["breadcrumbs"][0]["type"] == "project");
  REQUIRE(root_ctx["data"]["cards"].is_array());
  REQUIRE(root_ctx["data"]["cards"].size() == 2);
  bool root_has_a = false;
  bool root_has_b = false;
  for (const auto& card : root_ctx["data"]["cards"]) {
    if (card["card_id"] == "11111111-1111-4111-8111-111111111111") {
      root_has_a = true;
      REQUIRE(card["child_count"] == 1);
    }
    if (card["card_id"] == "22222222-2222-4222-8222-222222222222") {
      root_has_b = true;
      REQUIRE(card["child_count"] == 0);
    }
  }
  REQUIRE(root_has_a);
  REQUIRE(root_has_b);

  const auto child_ctx = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/context?project_id=proj-1&parent_card_id=11111111-1111-4111-8111-111111111111&count=true",
      nlohmann::json::object(),
      boost::beast::http::status::ok);
  REQUIRE(child_ctx["ok"] == true);
  REQUIRE(child_ctx["data"]["current_parent_card_id"] == "11111111-1111-4111-8111-111111111111");
  REQUIRE(child_ctx["data"]["breadcrumbs"].size() == 2);
  REQUIRE(child_ctx["data"]["breadcrumbs"][1]["type"] == "card");
  REQUIRE(child_ctx["data"]["breadcrumbs"][1]["card_id"] == "11111111-1111-4111-8111-111111111111");
  REQUIRE(child_ctx["data"]["cards"].size() == 1);
  REQUIRE(child_ctx["data"]["cards"][0]["card_id"] == "33333333-3333-4333-8333-333333333333");
  REQUIRE(child_ctx["data"]["cards"][0]["child_count"] == 1);

  const auto missing_parent = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/context?project_id=proj-1&parent_card_id=does-not-exist&count=true",
      nlohmann::json::object(),
      boost::beast::http::status::not_found);
  REQUIRE(missing_parent["ok"] == false);
  REQUIRE(missing_parent["error"]["code"] == "not_found");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP card move endpoint rejects invalid input", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto project_root_1 = dir / "project_repo_1";
  const auto project_root_2 = dir / "project_repo_2";
  create_project(db, "proj-1", project_root_1.string());
  create_project(db, "proj-2", project_root_2.string());
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

  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/cards",
                    {{"card_id", "11111111-1111-4111-8111-111111111111"},
                     {"project_id", "proj-1"},
                     {"title", "A"},
                     {"content", "A"},
                     {"created_at", 10},
                     {"updated_at", 10}},
                    boost::beast::http::status::created);
  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/cards",
                    {{"card_id", "22222222-2222-4222-8222-222222222222"},
                     {"project_id", "proj-1"},
                     {"title", "B"},
                     {"content", "B"},
                     {"created_at", 11},
                     {"updated_at", 11}},
                    boost::beast::http::status::created);
  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/cards",
                    {{"card_id", "33333333-3333-4333-8333-333333333333"},
                     {"project_id", "proj-2"},
                     {"title", "X"},
                     {"content", "X"},
                     {"created_at", 12},
                     {"updated_at", 12}},
                    boost::beast::http::status::created);

  const auto missing_intent = http_json_request(bound.bind,
                                                bound.port,
                                                token,
                                                boost::beast::http::verb::post,
                                                "/cards/11111111-1111-4111-8111-111111111111/move",
                                                {{"project_id", "proj-1"}},
                                                boost::beast::http::status::bad_request);
  REQUIRE(missing_intent["ok"] == false);
  REQUIRE(missing_intent["error"]["code"] == "bad_request");

  const auto missing_target = http_json_request(bound.bind,
                                                bound.port,
                                                token,
                                                boost::beast::http::verb::post,
                                                "/cards/11111111-1111-4111-8111-111111111111/move",
                                                {{"project_id", "proj-1"}, {"intent", "before"}},
                                                boost::beast::http::status::bad_request);
  REQUIRE(missing_target["ok"] == false);
  REQUIRE(missing_target["error"]["code"] == "missing_target_card_id");

  const auto cross_project = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/11111111-1111-4111-8111-111111111111/move",
      {{"project_id", "proj-1"}, {"intent", "into"}, {"target_card_id", "33333333-3333-4333-8333-333333333333"}},
      boost::beast::http::status::not_found);
  REQUIRE(cross_project["ok"] == false);
  REQUIRE(cross_project["error"]["code"] == "target_not_found");

  const auto bad_intent = http_json_request(bound.bind,
                                            bound.port,
                                            token,
                                            boost::beast::http::verb::post,
                                            "/cards/11111111-1111-4111-8111-111111111111/move",
                                            {{"project_id", "proj-1"}, {"intent", "teleport"}},
                                            boost::beast::http::status::bad_request);
  REQUIRE(bad_intent["ok"] == false);
  REQUIRE(bad_intent["error"]["code"] == "invalid_move_intent");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP cards view=recent listing", "[http]") {
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

  auto create_card = [&](const std::string& id, const std::string& title, int t) {
    return http_json_request(bound.bind,
                             bound.port,
                             token,
                             boost::beast::http::verb::post,
                             "/cards",
                             {
                                 {"card_id", id},
                                 {"project_id", "proj-1"},
                                 {"title", title},
                                 {"content", title},
                                 {"created_at", t},
                                 {"updated_at", t},
                             },
                             boost::beast::http::status::created);
  };

  REQUIRE(create_card("11111111-1111-4111-8111-111111111111", "First", 10)["ok"] == true);
  REQUIRE(create_card("22222222-2222-4222-8222-222222222222", "Second", 20)["ok"] == true);
  REQUIRE(create_card("33333333-3333-4333-8333-333333333333", "Third", 30)["ok"] == true);

  const auto deleted = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::delete_,
                                         "/cards/22222222-2222-4222-8222-222222222222",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  const auto overview = http_json_request(bound.bind,
                                          bound.port,
                                          token,
                                          boost::beast::http::verb::get,
                                          "/cards?project_id=proj-1&view=recent",
                                          nlohmann::json::object(),
                                          boost::beast::http::status::ok);
  REQUIRE(overview["ok"] == true);
  REQUIRE(overview["data"].is_array());
  REQUIRE(overview["data"].size() == 2);
  REQUIRE(overview["data"][0]["card_id"] == "33333333-3333-4333-8333-333333333333");
  REQUIRE(overview["data"][1]["card_id"] == "11111111-1111-4111-8111-111111111111");

  const auto overview_limit = http_json_request(bound.bind,
                                                bound.port,
                                                token,
                                                boost::beast::http::verb::get,
                                                "/cards?project_id=proj-1&view=recent&limit=1",
                                                nlohmann::json::object(),
                                                boost::beast::http::status::ok);
  REQUIRE(overview_limit["ok"] == true);
  REQUIRE(overview_limit["data"].is_array());
  REQUIRE(overview_limit["data"].size() == 1);
  REQUIRE(overview_limit["data"][0]["card_id"] == "33333333-3333-4333-8333-333333333333");

  const auto missing_project = http_json_request(bound.bind,
                                                 bound.port,
                                                 token,
                                                 boost::beast::http::verb::get,
                                                 "/cards?view=recent",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::bad_request);
  REQUIRE(missing_project["ok"] == false);
  REQUIRE(missing_project["error"]["code"] == "bad_request");

  const auto bad_limit = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::get,
                                           "/cards?project_id=proj-1&view=recent&limit=abc",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::bad_request);
  REQUIRE(bad_limit["ok"] == false);
  REQUIRE(bad_limit["error"]["code"] == "bad_request");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP cards/context support explicit order parameter", "[http]") {
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

  auto create_card = [&](const std::string& id, const std::string& title, int t) {
    return http_json_request(bound.bind,
                             bound.port,
                             token,
                             boost::beast::http::verb::post,
                             "/cards",
                             {
                                 {"card_id", id},
                                 {"project_id", "proj-1"},
                                 {"title", title},
                                 {"content", title},
                                 {"created_at", t},
                                 {"updated_at", t},
                             },
                             boost::beast::http::status::created);
  };

  REQUIRE(create_card("11111111-1111-4111-8111-111111111111", "Zulu", 10)["ok"] == true);
  REQUIRE(create_card("22222222-2222-4222-8222-222222222222", "Alpha", 20)["ok"] == true);
  REQUIRE(create_card("33333333-3333-4333-8333-333333333333", "Mike", 30)["ok"] == true);

  const auto tree_by_updated = http_json_request(bound.bind,
                                                 bound.port,
                                                 token,
                                                 boost::beast::http::verb::get,
                                                 "/cards?project_id=proj-1&view=tree&order=updated_desc",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::ok);
  REQUIRE(tree_by_updated["ok"] == true);
  REQUIRE(tree_by_updated["data"].size() == 3);
  REQUIRE(tree_by_updated["data"][0]["card_id"] == "33333333-3333-4333-8333-333333333333");
  REQUIRE(tree_by_updated["data"][1]["card_id"] == "22222222-2222-4222-8222-222222222222");
  REQUIRE(tree_by_updated["data"][2]["card_id"] == "11111111-1111-4111-8111-111111111111");

  const auto recent_by_title = http_json_request(bound.bind,
                                                 bound.port,
                                                 token,
                                                 boost::beast::http::verb::get,
                                                 "/cards?project_id=proj-1&view=recent&order=title_asc",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::ok);
  REQUIRE(recent_by_title["ok"] == true);
  REQUIRE(recent_by_title["data"].size() == 3);
  REQUIRE(recent_by_title["data"][0]["title"] == "Alpha");
  REQUIRE(recent_by_title["data"][1]["title"] == "Mike");
  REQUIRE(recent_by_title["data"][2]["title"] == "Zulu");

  const auto context_by_title = http_json_request(bound.bind,
                                                  bound.port,
                                                  token,
                                                  boost::beast::http::verb::get,
                                                  "/cards/context?project_id=proj-1&order=title_asc",
                                                  nlohmann::json::object(),
                                                  boost::beast::http::status::ok);
  REQUIRE(context_by_title["ok"] == true);
  REQUIRE(context_by_title["data"]["cards"].size() == 3);
  REQUIRE(context_by_title["data"]["cards"][0]["title"] == "Alpha");
  REQUIRE(context_by_title["data"]["cards"][1]["title"] == "Mike");
  REQUIRE(context_by_title["data"]["cards"][2]["title"] == "Zulu");
  REQUIRE(!context_by_title["data"]["cards"][0].contains("child_count"));

  const auto context_with_counts = http_json_request(bound.bind,
                                                     bound.port,
                                                     token,
                                                     boost::beast::http::verb::get,
                                                     "/cards/context?project_id=proj-1&order=title_asc&count=true",
                                                     nlohmann::json::object(),
                                                     boost::beast::http::status::ok);
  REQUIRE(context_with_counts["ok"] == true);
  REQUIRE(context_with_counts["data"]["cards"][0].contains("child_count"));

  const auto bad_order = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::get,
                                           "/cards?project_id=proj-1&view=tree&order=bogus",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::bad_request);
  REQUIRE(bad_order["ok"] == false);
  REQUIRE(bad_order["error"]["code"] == "bad_request");

  const auto bad_context_order = http_json_request(bound.bind,
                                                   bound.port,
                                                   token,
                                                   boost::beast::http::verb::get,
                                                   "/cards/context?project_id=proj-1&order=bogus",
                                                   nlohmann::json::object(),
                                                   boost::beast::http::status::bad_request);
  REQUIRE(bad_context_order["ok"] == false);
  REQUIRE(bad_context_order["error"]["code"] == "bad_request");

  const auto bad_count = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::get,
                                           "/cards?project_id=proj-1&view=tree&count=maybe",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::bad_request);
  REQUIRE(bad_count["ok"] == false);
  REQUIRE(bad_count["error"]["code"] == "bad_request");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP cards view=recent listing (alias coverage)", "[http]") {
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

  auto create_card = [&](const std::string& id, const std::string& title, int t) {
    return http_json_request(bound.bind,
                             bound.port,
                             token,
                             boost::beast::http::verb::post,
                             "/cards",
                             {
                                 {"card_id", id},
                                 {"project_id", "proj-1"},
                                 {"title", title},
                                 {"content", title},
                                 {"created_at", t},
                                 {"updated_at", t},
                             },
                             boost::beast::http::status::created);
  };

  REQUIRE(create_card("11111111-1111-4111-8111-111111111111", "First", 10)["ok"] == true);
  REQUIRE(create_card("22222222-2222-4222-8222-222222222222", "Second", 20)["ok"] == true);
  REQUIRE(create_card("33333333-3333-4333-8333-333333333333", "Third", 30)["ok"] == true);

  const auto deleted = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::delete_,
                                         "/cards/22222222-2222-4222-8222-222222222222",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  const auto recent = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::get,
                                        "/cards?project_id=proj-1&view=recent",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(recent["ok"] == true);
  REQUIRE(recent["data"].is_array());
  REQUIRE(recent["data"].size() == 2);
  REQUIRE(recent["data"][0]["card_id"] == "33333333-3333-4333-8333-333333333333");
  REQUIRE(recent["data"][1]["card_id"] == "11111111-1111-4111-8111-111111111111");

  const auto recent_limit = http_json_request(bound.bind,
                                              bound.port,
                                              token,
                                              boost::beast::http::verb::get,
                                              "/cards?project_id=proj-1&view=recent&limit=1",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(recent_limit["ok"] == true);
  REQUIRE(recent_limit["data"].is_array());
  REQUIRE(recent_limit["data"].size() == 1);
  REQUIRE(recent_limit["data"][0]["card_id"] == "33333333-3333-4333-8333-333333333333");

  const auto missing_project = http_json_request(bound.bind,
                                                 bound.port,
                                                 token,
                                                 boost::beast::http::verb::get,
                                                 "/cards?view=recent",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::bad_request);
  REQUIRE(missing_project["ok"] == false);
  REQUIRE(missing_project["error"]["code"] == "bad_request");

  const auto bad_limit = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::get,
                                           "/cards?project_id=proj-1&view=recent&limit=abc",
                                           nlohmann::json::object(),
                                           boost::beast::http::status::bad_request);
  REQUIRE(bad_limit["ok"] == false);
  REQUIRE(bad_limit["error"]["code"] == "bad_request");

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
