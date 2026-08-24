#include "http_test_helpers.h"

#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "model/AiMessage.h"
#include "model/AiThread.h"
#include "model/Resource.h"
#include "resource/ResourceRepo.h"

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
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });

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

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards",
      card_a,
      boost::beast::http::status::created
  );
  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards",
      card_b,
      boost::beast::http::status::created
  );
  nlohmann::json link_body = {
      {"to_card_id", "card-b"},
      {"to_type", "card"},
      {"kind", "ref"},
      {"label", "See"},
      {"created_at", 123}
  };
  const auto created = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/card-a/links",
      link_body,
      boost::beast::http::status::created
  );
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["to_card_id"] == "card-b");
  REQUIRE(created["data"]["to_type"] == "card");
  REQUIRE(created["data"]["kind"] == "ref");
  REQUIRE(created["data"]["label"] == "See");

  const auto listed = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/links",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["to_card_id"] == "card-b");
  REQUIRE(listed["data"][0]["to_type"] == "card");
  REQUIRE(listed["data"][0]["kind"] == "ref");
  REQUIRE(listed["data"][0]["label"] == "See");

  const auto backlinks = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-b/backlinks",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(backlinks["ok"] == true);
  REQUIRE(backlinks["data"].is_array());
  REQUIRE(backlinks["data"].size() == 1);
  REQUIRE(backlinks["data"][0]["from_card_id"] == "card-a");

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/cards/card-b",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );

  const auto hidden = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/links",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(hidden["data"].size() == 0);

  const auto visible = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/links?include_deleted=1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(visible["data"].size() == 1);

  nlohmann::json delete_body = {{"to_card_id", "card-b"}, {"kind", "ref"}};
  const auto deleted = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/cards/card-a/links",
      delete_body,
      boost::beast::http::status::ok
  );
  REQUIRE(deleted["ok"] == true);

  const auto listed_after = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/links",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(listed_after["data"].size() == 0);

  nlohmann::json invalid_body = {
      {"to_card_id", "missing-card"},
      {"to_type", "card"},
      {"kind", "ref"}
  };
  const auto invalid = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/card-a/links",
      invalid_body,
      boost::beast::http::status::bad_request
  );
  REQUIRE(invalid["ok"] == false);

  server.stop();
  server_thread.join();
}

TEST_CASE("HTTP card links validate non-card targets and filter ai-message sources", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto project_root = dir / "project_repo";
  const auto other_project_root = dir / "project_repo_2";
  create_project(db, "proj-1", project_root.string());
  create_project(db, "proj-2", other_project_root.string());
  ensure_uuid_seeded();

  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  holder::ai::AiThreadRepo thread_repo(db);
  holder::model::AiThread thread_1;
  thread_1.thread_id = "thread-1";
  thread_1.project_id = "proj-1";
  thread_1.title = "Thread 1";
  thread_1.created_at = 1;
  thread_1.updated_at = 1;
  thread_repo.create(thread_1);

  holder::model::AiThread thread_2;
  thread_2.thread_id = "thread-2";
  thread_2.project_id = "proj-2";
  thread_2.title = "Thread 2";
  thread_2.created_at = 2;
  thread_2.updated_at = 2;
  thread_repo.create(thread_2);

  holder::ai::AiMessageRepo msg_repo(db, &fts);
  holder::model::AiMessage msg_1;
  msg_1.message_id = "msg-1";
  msg_1.thread_id = "thread-1";
  msg_1.role = "user";
  msg_1.source = "manual";
  msg_1.content = "m1";
  msg_1.created_at = 3;
  msg_repo.append(msg_1);

  holder::model::AiMessage msg_2 = msg_1;
  msg_2.message_id = "msg-2";
  msg_2.content = "m2";
  msg_2.created_at = 4;
  msg_repo.append(msg_2);

  holder::model::AiMessage msg_3 = msg_1;
  msg_3.message_id = "msg-3";
  msg_3.thread_id = "thread-2";
  msg_3.content = "m3";
  msg_3.created_at = 5;
  msg_repo.append(msg_3);

  holder::resource::ResourceRepo resource_repo(db);
  holder::model::Resource resource_1;
  resource_1.resource_id = "res-1";
  resource_1.project_id = "proj-1";
  resource_1.type = "website";
  resource_1.metadata["identifier"] = {"https://example.com/1"};
  resource_1.label = "R1";
  resource_1.created_at = 10;
  resource_1.updated_at = 10;
  resource_repo.add(resource_1);

  holder::model::Resource resource_2 = resource_1;
  resource_2.resource_id = "res-2";
  resource_2.project_id = "proj-2";
  resource_2.metadata["identifier"] = {"https://example.com/2"};
  resource_2.label = "R2";
  resource_2.created_at = 11;
  resource_2.updated_at = 11;
  resource_repo.add(resource_2);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, &fts);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  nlohmann::json card_a = {
      {"card_id", "card-a"},
      {"project_id", "proj-1"},
      {"title", "Card A"},
      {"content", "alpha"},
      {"created_at", 12},
      {"updated_at", 12}
  };
  nlohmann::json card_b = {
      {"card_id", "card-b"},
      {"project_id", "proj-1"},
      {"title", "Card B"},
      {"content", "beta"},
      {"created_at", 13},
      {"updated_at", 13}
  };
  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards",
      card_a,
      boost::beast::http::status::created
  );
  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards",
      card_b,
      boost::beast::http::status::created
  );
  nlohmann::json card_c_other_project = {
      {"card_id", "card-c"},
      {"project_id", "proj-2"},
      {"title", "Card C"},
      {"content", "gamma"},
      {"created_at", 14},
      {"updated_at", 14}
  };
  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards",
      card_c_other_project,
      boost::beast::http::status::created
  );

  auto create_link = [&](const nlohmann::json& body, boost::beast::http::status status) {
    return http_json_request(
        bound.bind,
        bound.port,
        token,
        boost::beast::http::verb::post,
        "/cards/card-a/links",
        body,
        status
    );
  };

  const auto link_ai_thread = create_link(
      {{"to_card_id", "thread-1"}, {"to_type", "ai_thread"}},
      boost::beast::http::status::created
  );
  REQUIRE(link_ai_thread["ok"] == true);
  REQUIRE(link_ai_thread["data"]["to_type"] == "ai_thread");

  const auto link_resource = create_link(
      {{"to_card_id", "res-1"}, {"to_type", "resource"}},
      boost::beast::http::status::created
  );
  REQUIRE(link_resource["ok"] == true);
  REQUIRE(link_resource["data"]["to_type"] == "resource");

  const auto link_ai_message = create_link(
      {{"to_card_id", "msg-2"}, {"to_type", "ai_message"}},
      boost::beast::http::status::created
  );
  REQUIRE(link_ai_message["ok"] == true);
  REQUIRE(link_ai_message["data"]["to_type"] == "ai_message");

  const auto missing_target_id = create_link(
      {{"to_card_id", ""}, {"to_type", "ai_thread"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(missing_target_id["error"]["message"] == "Missing to_card_id.");

  const auto thread_other_project = create_link(
      {{"to_card_id", "thread-2"}, {"to_type", "ai_thread"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(
      thread_other_project["error"]["message"] == "Target ai thread is in a different project."
  );
  const auto thread_missing = create_link(
      {{"to_card_id", "thread-missing"}, {"to_type", "ai_thread"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(thread_missing["error"]["message"] == "Target ai thread not found.");

  const auto resource_other_project = create_link(
      {{"to_card_id", "res-2"}, {"to_type", "resource"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(
      resource_other_project["error"]["message"] == "Target resource is in a different project."
  );
  const auto resource_missing = create_link(
      {{"to_card_id", "res-missing"}, {"to_type", "resource"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(resource_missing["error"]["message"] == "Target resource not found.");

  const auto message_other_project = create_link(
      {{"to_card_id", "msg-3"}, {"to_type", "ai_message"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(
      message_other_project["error"]["message"] == "Target ai message is in a different project."
  );
  const auto message_missing = create_link(
      {{"to_card_id", "msg-missing"}, {"to_type", "ai_message"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(message_missing["error"]["message"] == "Target ai message not found.");
  const auto card_other_project = create_link(
      {{"to_card_id", "card-c"}, {"to_type", "card"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(card_other_project["error"]["message"] == "Target card is in a different project.");

  db.exec("PRAGMA foreign_keys = OFF;");
  db.exec("INSERT INTO ai_messages(message_id, thread_id, role, source, content, created_at) "
          "VALUES('msg-orphan', 'thread-missing', 'user', 'manual', 'orphan', 20);");
  db.exec("PRAGMA foreign_keys = ON;");
  const auto orphan_thread = create_link(
      {{"to_card_id", "msg-orphan"}, {"to_type", "ai_message"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(orphan_thread["error"]["message"] == "Target ai message thread not found.");

  const auto unsupported = create_link(
      {{"to_card_id", "whatever"}, {"to_type", "custom_type"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(unsupported["error"]["message"] == "Unsupported to_type.");

  const auto list_outgoing = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/links",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(list_outgoing["data"].size() >= 3);

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/ai/messages/msg-2",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );

  const auto filtered_outgoing = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/links",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  bool saw_deleted_message_link = false;
  for (const auto& item : filtered_outgoing["data"]) {
    if (item["to_type"] == "ai_message" && item["to_card_id"] == "msg-2") {
      saw_deleted_message_link = true;
    }
  }
  REQUIRE(saw_deleted_message_link == false);

  const auto include_deleted_outgoing = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/links?include_deleted=1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  bool saw_included_deleted_message_link = false;
  for (const auto& item : include_deleted_outgoing["data"]) {
    if (item["to_type"] == "ai_message" && item["to_card_id"] == "msg-2") {
      saw_included_deleted_message_link = true;
    }
  }
  REQUIRE(saw_included_deleted_message_link);

  const auto ai_to_card = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/ai/messages/msg-1/links",
      {{"to_card_id", "card-a"}, {"to_type", "card"}},
      boost::beast::http::status::created
  );
  REQUIRE(ai_to_card["ok"] == true);

  const auto card_backlinks = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/backlinks",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  bool saw_msg_source = false;
  for (const auto& item : card_backlinks["data"]) {
    if (item["from_card_id"] == "msg-1") {
      saw_msg_source = true;
    }
  }
  REQUIRE(saw_msg_source);

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/ai/messages/msg-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );

  const auto filtered_backlinks = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/backlinks",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  bool saw_deleted_msg_source = false;
  for (const auto& item : filtered_backlinks["data"]) {
    if (item["from_card_id"] == "msg-1") {
      saw_deleted_msg_source = true;
    }
  }
  REQUIRE(saw_deleted_msg_source == false);

  const auto include_deleted_backlinks = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/backlinks?include_deleted=1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  bool saw_included_deleted_msg_source = false;
  for (const auto& item : include_deleted_backlinks["data"]) {
    if (item["from_card_id"] == "msg-1") {
      saw_included_deleted_msg_source = true;
    }
  }
  REQUIRE(saw_included_deleted_msg_source);

  server.stop();
  server_thread.join();
}
