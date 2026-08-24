#include "api/routes/ai/messages/AiMessageLinkRoutes.h"
#include "http_test_helpers.h"

#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "card/CardRepo.h"
#include "card/LinkRepo.h"
#include "model/AiThread.h"
#include "model/Card.h"
#include "model/CardLink.h"
#include "model/Resource.h"
#include "resource/ResourceRepo.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

namespace {

namespace http = boost::beast::http;

http::request<http::string_body> make_route_request(
    http::verb method,
    const std::string& path,
    const std::string& body = ""
) {
  http::request<http::string_body> req{method, path, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body;
  }
  return req;
}

void create_card(
    holder::platform::Db& db,
    const std::string& id,
    const std::string& project_id,
    bool deleted
) {
  holder::card::CardRepo repo(db);
  holder::model::Card card;
  card.card_id = id;
  card.project_id = project_id;
  card.title = id;
  card.rel_path = "cards/" + id + ".md";
  card.sort_key = 1.0;
  card.created_at = 1;
  card.updated_at = 1;
  repo.create(card);
  if (deleted) {
    repo.soft_delete(id, 99, 99);
  }
}

void upsert_link(
    holder::card::LinkRepo& link_repo,
    const std::string& project_id,
    const std::string& from_id,
    const std::string& to_id,
    const std::string& to_type,
    const std::string& kind = "ref"
) {
  holder::model::CardLink link;
  link.project_id = project_id;
  link.from_card_id = from_id;
  link.to_card_id = to_id;
  link.to_type = to_type;
  link.kind = kind;
  link.created_at = 1;
  link_repo.upsert_links(project_id, from_id, {link});
}

} // namespace

TEST_CASE("HTTP ai message links create/list/backlinks", "[http]") {
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
  holder::ai::AiThreadRepo thread_repo(db);
  thread_repo.create(thread);

  holder::index::FtsIndexer fts(db);
  holder::ai::AiMessageRepo msg_repo(db, &fts);

  holder::model::AiMessage msg1;
  msg1.message_id = "msg-1";
  msg1.thread_id = "thread-1";
  msg1.role = "user";
  msg1.source = "manual";
  msg1.content = "Hello";
  msg1.created_at = 2;
  msg_repo.append(msg1);

  holder::model::AiMessage msg2 = msg1;
  msg2.message_id = "msg-2";
  msg2.content = "Hi";
  msg2.created_at = 3;
  msg_repo.append(msg2);

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

  nlohmann::json link_body = {{"to_card_id", "msg-2"}, {"to_type", "ai_message"}, {"kind", "ref"}};
  const auto created = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/ai/messages/msg-1/links",
      link_body,
      boost::beast::http::status::created
  );
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["to_type"] == "ai_message");

  const auto listed = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/messages/msg-1/links",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["to_card_id"] == "msg-2");
  REQUIRE(listed["data"][0]["to_type"] == "ai_message");

  const auto backlinks = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/messages/msg-2/backlinks",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(backlinks["ok"] == true);
  REQUIRE(backlinks["data"].is_array());
  REQUIRE(backlinks["data"].size() == 1);
  REQUIRE(backlinks["data"][0]["from_card_id"] == "msg-1");

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/ai/messages/msg-2",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );

  const auto hidden = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/messages/msg-1/links",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(hidden["data"].size() == 0);

  const auto visible = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/messages/msg-1/links?include_deleted=1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(visible["data"].size() == 1);

  nlohmann::json invalid_body = {{"to_card_id", "msg-2"}, {"to_type", "unknown"}, {"kind", "ref"}};
  const auto invalid = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/ai/messages/msg-1/links",
      invalid_body,
      boost::beast::http::status::bad_request
  );
  REQUIRE(invalid["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("AiMessageLinkRoutes direct route validates and handles malformed requests", "[http]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());

  holder::ai::AiThreadRepo thread_repo(db);
  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  thread_repo.create(thread);

  holder::index::FtsIndexer fts(db);
  holder::ai::AiMessageRepo msg_repo(db, &fts);
  holder::model::AiMessage msg;
  msg.message_id = "msg-1";
  msg.thread_id = "thread-1";
  msg.role = "user";
  msg.source = "manual";
  msg.content = "hello";
  msg.created_at = 2;
  msg_repo.append(msg);

  http::response<http::string_body> res;
  const auto no_query = [](const std::string&) {
    return std::string();
  };

  {
    auto req = make_route_request(http::verb::get, "/ai/messages//links");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages//links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::not_found);
  }

  {
    auto req = make_route_request(http::verb::put, "/ai/messages/msg-1/links");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::method_not_allowed);
  }

  {
    auto req = make_route_request(http::verb::post, "/ai/messages/msg-1/backlinks");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/backlinks",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::method_not_allowed);
  }

  {
    auto req = make_route_request(http::verb::post, "/ai/messages/msg-1/links", "{");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
  }

  {
    auto req = make_route_request(http::verb::delete_, "/ai/messages/msg-1/links", "{");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
  }

  {
    auto req = make_route_request(http::verb::get, "/ai/messages/missing/links");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/missing/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::not_found);
  }

  {
    auto req = make_route_request(http::verb::get, "/ai/messages/missing/backlinks");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/missing/backlinks",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::not_found);
  }
}

TEST_CASE("AiMessageLinkRoutes direct route filters links and validates target types", "[http]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  create_project(db, "proj-2", (dir / "project_repo_2").string());

  holder::index::FtsIndexer fts(db);
  holder::ai::AiThreadRepo thread_repo(db);
  holder::model::AiThread t1{"thread-1", "proj-1", std::nullopt, "T1", 1, 1};
  holder::model::AiThread t2{"thread-2", "proj-2", std::nullopt, "T2", 1, 1};
  thread_repo.create(t1);
  thread_repo.create(t2);

  holder::ai::AiMessageRepo msg_repo(db, &fts);
  holder::model::AiMessage msg;
  msg.role = "user";
  msg.source = "manual";
  msg.content = "x";
  msg.created_at = 10;
  msg.thread_id = "thread-1";
  msg.message_id = "msg-1";
  msg_repo.append(msg);
  msg.message_id = "msg-2";
  msg_repo.append(msg);
  msg.message_id = "msg-del";
  msg_repo.append(msg);
  db.exec("UPDATE ai_messages SET deleted_at=100 WHERE message_id='msg-del';");

  msg.thread_id = "thread-2";
  msg.message_id = "msg-other-project";
  msg_repo.append(msg);

  db.exec("PRAGMA foreign_keys=OFF;");
  db.exec("INSERT INTO ai_messages(message_id,thread_id,role,source,content,created_at) "
          "VALUES('msg-orphan','thread-missing','user','manual','orphan',11);");
  db.exec("PRAGMA foreign_keys=ON;");

  create_card(db, "card-1", "proj-1", false);
  create_card(db, "card-del", "proj-1", true);
  create_card(db, "card-other-project", "proj-2", false);

  holder::resource::ResourceRepo resource_repo(db);
  holder::model::Resource r1{
      .resource_id = "res-1",
      .project_id = "proj-1",
      .type = "website",
      .label = "R1",
      .metadata = {{"identifier", {"https://example.com/1"}}},
      .created_at = 1,
      .updated_at = 1,
  };
  holder::model::Resource r2{
      .resource_id = "res-2",
      .project_id = "proj-2",
      .type = "website",
      .label = "R2",
      .metadata = {{"identifier", {"https://example.com/2"}}},
      .created_at = 1,
      .updated_at = 1,
  };
  resource_repo.add(r1);
  resource_repo.add(r2);

  holder::card::LinkRepo link_repo(db);
  upsert_link(link_repo, "proj-1", "msg-1", "card-1", "card");
  upsert_link(link_repo, "proj-1", "msg-1", "card-del", "card");
  upsert_link(link_repo, "proj-1", "msg-1", "missing-card", "card");
  upsert_link(link_repo, "proj-1", "msg-1", "msg-2", "ai_message");
  upsert_link(link_repo, "proj-1", "msg-1", "msg-del", "ai_message");
  upsert_link(link_repo, "proj-1", "msg-1", "custom-target", "custom");

  upsert_link(link_repo, "proj-1", "card-1", "msg-1", "ai_message");
  upsert_link(link_repo, "proj-1", "card-del", "msg-1", "ai_message");
  upsert_link(link_repo, "proj-1", "msg-2", "msg-1", "ai_message");
  upsert_link(link_repo, "proj-1", "missing-source", "msg-1", "ai_message");

  http::response<http::string_body> res;
  auto no_query = [](const std::string&) {
    return std::string();
  };
  auto include_deleted = [](const std::string& key) {
    return key == "include_deleted" ? std::string("1") : std::string();
  };

  {
    auto req = make_route_request(http::verb::get, "/ai/messages/msg-1/links");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::ok);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"].size() == 3);
  }

  {
    auto req = make_route_request(http::verb::get, "/ai/messages/msg-1/links?include_deleted=1");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        include_deleted
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::ok);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"].size() == 6);
  }

  {
    auto req = make_route_request(http::verb::get, "/ai/messages/msg-1/backlinks");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/backlinks",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::ok);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"].size() == 2);
  }

  {
    auto req =
        make_route_request(http::verb::get, "/ai/messages/msg-1/backlinks?include_deleted=1");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/backlinks",
        req,
        res,
        db,
        &fts,
        include_deleted
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::ok);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"].size() == 4);
  }

  auto expect_bad_request_message = [&](const nlohmann::json& body,
                                        const std::string& expected_message) {
    auto req = make_route_request(http::verb::post, "/ai/messages/msg-1/links", body.dump());
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == expected_message);
  };

  expect_bad_request_message(nlohmann::json::object(), "Missing to_card_id.");
  expect_bad_request_message({{"to_card_id", ""}, {"to_type", "card"}}, "Missing to_card_id.");
  expect_bad_request_message(
      {{"to_card_id", "missing-card"}, {"to_type", "card"}},
      "Target card not found."
  );
  expect_bad_request_message(
      {{"to_card_id", "card-other-project"}, {"to_type", "card"}},
      "Target card is in a different project."
  );
  expect_bad_request_message(
      {{"to_card_id", "missing-thread"}, {"to_type", "ai_thread"}},
      "Target ai thread not found."
  );
  expect_bad_request_message(
      {{"to_card_id", "thread-2"}, {"to_type", "ai_thread"}},
      "Target ai thread is in a different project."
  );
  expect_bad_request_message(
      {{"to_card_id", "missing-res"}, {"to_type", "resource"}},
      "Target resource not found."
  );
  expect_bad_request_message(
      {{"to_card_id", "res-2"}, {"to_type", "resource"}},
      "Target resource is in a different project."
  );
  expect_bad_request_message(
      {{"to_card_id", "missing-msg"}, {"to_type", "ai_message"}},
      "Target ai message not found."
  );
  expect_bad_request_message(
      {{"to_card_id", "msg-orphan"}, {"to_type", "ai_message"}},
      "Target ai message thread not found."
  );
  expect_bad_request_message(
      {{"to_card_id", "msg-other-project"}, {"to_type", "ai_message"}},
      "Target ai message is in a different project."
  );

  {
    auto req = make_route_request(
        http::verb::post,
        "/ai/messages/msg-1/links",
        nlohmann::json({{"to_card_id", "thread-1"}, {"to_type", "ai_thread"}}).dump()
    );
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::created);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["to_type"] == "ai_thread");
  }

  {
    auto req = make_route_request(
        http::verb::post,
        "/ai/messages/msg-1/links",
        nlohmann::json({{"to_card_id", "res-1"}, {"to_type", "resource"}}).dump()
    );
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::created);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["to_type"] == "resource");
  }

  {
    auto req = make_route_request(http::verb::get, "/ai/messages/msg-orphan/links");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-orphan/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Thread not found.");
  }

  {
    auto req = make_route_request(http::verb::get, "/ai/messages/msg-orphan/backlinks");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-orphan/backlinks",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Thread not found.");
  }

  {
    auto req = make_route_request(
        http::verb::post,
        "/ai/messages/msg-1/links",
        nlohmann::json({{"to_card_id", "card-1"}, {"to_type", nullptr}, {"kind", nullptr}}).dump()
    );
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::created);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["to_type"] == "card");
    REQUIRE(payload["data"]["kind"] == "ref");
  }

  {
    auto req = make_route_request(
        http::verb::post,
        "/ai/messages/msg-1/links",
        nlohmann::json({{"to_card_id", "card-1"},
                        {"to_type", "card"},
                        {"kind", "ref"},
                        {"label", "L"},
                        {"created_at", 777}}
        ).dump()
    );
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::created);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["label"] == "L");
    REQUIRE(payload["data"]["created_at"] == 777);
  }

  {
    auto req = make_route_request(
        http::verb::delete_,
        "/ai/messages/msg-1/links",
        nlohmann::json({{"to_card_id", "card-1"}, {"to_type", "card"}, {"kind", "ref"}}).dump()
    );
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::ok);
  }

  {
    auto req = make_route_request(http::verb::delete_, "/ai/messages/msg-1/links");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/links",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::ok);
  }

  {
    db.exec("DROP TABLE ai_messages;");
    auto req = make_route_request(http::verb::get, "/ai/messages/msg-1/backlinks");
    const bool handled = holder::api::routes::ai::messages::handle_ai_message_link_routes(
        "/ai/messages/msg-1/backlinks",
        req,
        res,
        db,
        &fts,
        no_query
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
  }
}
