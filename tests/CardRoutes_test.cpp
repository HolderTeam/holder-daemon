#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/AiThreadRepo.h"
#include "api/routes/CardRoutes.h"
#include "card/CardStore.h"
#include "card/LinkRepo.h"
#include "http_test_helpers.h"
#include "index/FtsIndexer.h"
#include "resource/ResourceRepo.h"

#include "model/AiThread.h"
#include "model/Resource.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <unordered_map>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(
    http::verb method,
    const std::string& target,
    const std::string& body = ""
) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  req.body() = body;
  req.prepare_payload();
  return req;
}

std::function<std::string(const std::string&)> map_param_getter(
    const std::unordered_map<std::string, std::string>& params
) {
  return [params](const std::string& key) {
    const auto it = params.find(key);
    return it == params.end() ? std::string() : it->second;
  };
}

} // namespace

TEST_CASE("CardRoutes returns false for non-card paths", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto req = make_request(http::verb::get, "/not-cards");
  http::response<http::string_body> res;
  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  const bool handled = holder::api::routes::handle_card_routes(
      "/not-cards",
      req,
      res,
      db,
      nullptr,
      nullptr,
      uuid_v4,
      param_get
  );
  REQUIRE_FALSE(handled);
}

TEST_CASE("CardRoutes context errors for missing and unknown project", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };

  SECTION("missing project_id") {
    auto req = make_request(http::verb::get, "/cards/context");
    http::response<http::string_body> res;
    const auto param_get = map_param_getter({});

    const bool handled = holder::api::routes::handle_card_routes(
        "/cards/context",
        req,
        res,
        db,
        nullptr,
        nullptr,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("project not found") {
    auto req = make_request(http::verb::get, "/cards/context");
    http::response<http::string_body> res;
    const auto param_get = map_param_getter({{"project_id", "missing-proj"}});

    const bool handled = holder::api::routes::handle_card_routes(
        "/cards/context",
        req,
        res,
        db,
        nullptr,
        nullptr,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::not_found);
  }
}

TEST_CASE("CardRoutes recent view validates limit param", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  holder::test::create_project(db, "proj-1", (dir / "repo").string());

  auto req = make_request(http::verb::get, "/cards");
  http::response<http::string_body> res;
  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = map_param_getter({
      {"project_id", "proj-1"},
      {"view", "recent"},
      {"limit", "0"},
  });

  const bool handled = holder::api::routes::handle_card_routes(
      "/cards",
      req,
      res,
      db,
      nullptr,
      nullptr,
      uuid_v4,
      param_get
  );
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("CardRoutes post returns not_implemented without card_store", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto body = nlohmann::json{
      {"project_id", "proj-1"},
      {"title", "T"},
      {"content", "C"},
  };
  auto req = make_request(http::verb::post, "/cards", body.dump());
  http::response<http::string_body> res;
  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  const bool handled = holder::api::routes::handle_card_routes(
      "/cards",
      req,
      res,
      db,
      nullptr,
      nullptr,
      uuid_v4,
      param_get
  );
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::not_implemented);
}

TEST_CASE("CardRoutes move intent edge branches", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", (dir / "repo1").string());
  holder::test::create_project(db, "proj-2", (dir / "repo2").string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  auto call = [&](http::verb method, const std::string& path, const nlohmann::json& body) {
    auto req = make_request(method, path, body.dump());
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        path,
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    return nlohmann::json::parse(res.body());
  };

  auto create = [&](const std::string& card_id,
                    const std::string& project_id,
                    const std::string& title,
                    int t,
                    const nlohmann::json& extra = nlohmann::json::object()) {
    nlohmann::json body{
        {"card_id", card_id},
        {"project_id", project_id},
        {"title", title},
        {"content", title},
        {"created_at", t},
        {"updated_at", t},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
      body[it.key()] = it.value();
    }
    auto req = make_request(http::verb::post, "/cards", body.dump());
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        "/cards",
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::created);
  };

  create("11111111-1111-4111-8111-111111111111", "proj-1", "A", 10);
  create("22222222-2222-4222-8222-222222222222", "proj-1", "B", 11);
  create("33333333-3333-4333-8333-333333333333", "proj-1", "C", 12);
  create("44444444-4444-4444-8444-444444444444", "proj-1", "D", 13);
  create("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "proj-2", "X", 20);

  SECTION("empty card id route") {
    auto req = make_request(
        http::verb::post,
        "/cards//move",
        R"({"project_id":"proj-1","intent":"to_end"})"
    );
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        "/cards//move",
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::not_found);
  }

  SECTION("move route unavailable when card_store is null") {
    auto req = make_request(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/move",
        R"({"project_id":"proj-1","intent":"to_end"})"
    );
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        "/cards/11111111-1111-4111-8111-111111111111/move",
        req,
        res,
        db,
        nullptr,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::not_implemented);
  }

  SECTION("move route method not allowed") {
    auto req = make_request(http::verb::get, "/cards/11111111-1111-4111-8111-111111111111/move");
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        "/cards/11111111-1111-4111-8111-111111111111/move",
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::method_not_allowed);
  }

  SECTION("source not found") {
    const auto payload = call(
        http::verb::post,
        "/cards/ffffffff-ffff-4fff-8fff-ffffffffffff/move",
        {{"project_id", "proj-1"}, {"intent", "to_end"}}
    );
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("source in different project") {
    const auto payload = call(
        http::verb::post,
        "/cards/aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa/move",
        {{"project_id", "proj-1"}, {"intent", "to_end"}}
    );
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "cross_project_move_forbidden");
  }

  SECTION("parent_card_id provided but parent not found") {
    const auto payload = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/move",
        {{"project_id", "proj-1"}, {"intent", "to_end"}, {"parent_card_id", "missing-parent"}}
    );
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "target_not_found");
  }

  SECTION("left no-op when source not among inferred siblings") {
    create("55555555-5555-4555-8555-555555555555", "proj-1", "Parent", 30);
    const auto payload = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/move",
        {{"project_id", "proj-1"},
         {"intent", "left"},
         {"parent_card_id", "55555555-5555-4555-8555-555555555555"}}
    );
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["card_id"] == "11111111-1111-4111-8111-111111111111");
  }

  SECTION("right no-op when already at end") {
    const auto payload = call(
        http::verb::post,
        "/cards/44444444-4444-4444-8444-444444444444/move",
        {{"project_id", "proj-1"}, {"intent", "right"}}
    );
    REQUIRE(payload["ok"] == true);
  }

  SECTION("left uses previous sibling target") {
    const auto payload = call(
        http::verb::post,
        "/cards/33333333-3333-4333-8333-333333333333/move",
        {{"project_id", "proj-1"}, {"intent", "left"}}
    );
    REQUIRE(payload["ok"] == true);
  }

  SECTION("up_level rejects root card") {
    const auto payload = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/move",
        {{"project_id", "proj-1"}, {"intent", "up_level"}}
    );
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "invalid_move_intent");
  }

  SECTION("up_level can set moved_into_title when grandparent exists") {
    create("66666666-6666-4666-8666-666666666666", "proj-1", "GP", 40);
    create(
        "77777777-7777-4777-8777-777777777777",
        "proj-1",
        "P",
        41,
        {{"parent_card_id", "66666666-6666-4666-8666-666666666666"}}
    );
    create(
        "88888888-8888-4888-8888-888888888888",
        "proj-1",
        "S",
        42,
        {{"parent_card_id", "77777777-7777-4777-8777-777777777777"}}
    );

    const auto payload = call(
        http::verb::post,
        "/cards/88888888-8888-4888-8888-888888888888/move",
        {{"project_id", "proj-1"}, {"intent", "up_level"}}
    );
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["parent_card_id"] == "66666666-6666-4666-8666-666666666666");
    REQUIRE(payload["data"]["moved_into_title"] == "GP");
  }

  SECTION("before with self target returns invalid_target") {
    const auto payload = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/move",
        {{"project_id", "proj-1"},
         {"intent", "before"},
         {"target_card_id", "11111111-1111-4111-8111-111111111111"}}
    );
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "invalid_target");
  }
}

TEST_CASE("CardRoutes links and backlinks edge branches", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", (dir / "repo1").string());
  holder::test::create_project(db, "proj-2", (dir / "repo2").string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto empty_param_get = [](const std::string&) {
    return std::string();
  };

  auto call_with_param_get = [&](http::verb method,
                                 const std::string& path,
                                 const nlohmann::json& body,
                                 const std::function<std::string(const std::string&)>& param_get) {
    auto req = make_request(method, path, body.dump());
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        path,
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };
  auto call = [&](http::verb method, const std::string& path, const nlohmann::json& body) {
    return call_with_param_get(method, path, body, empty_param_get);
  };

  auto create_card =
      [&](const std::string& card_id, const std::string& project_id, const std::string& title, int t
      ) {
        auto [status, payload] = call(
            http::verb::post,
            "/cards",
            {
                {"card_id", card_id},
                {"project_id", project_id},
                {"title", title},
                {"content", title},
                {"created_at", t},
                {"updated_at", t},
            }
        );
        REQUIRE(status == http::status::created);
        REQUIRE(payload["ok"] == true);
      };

  create_card("11111111-1111-4111-8111-111111111111", "proj-1", "A", 10);
  create_card("22222222-2222-4222-8222-222222222222", "proj-1", "B", 11);

  holder::ai::AiThreadRepo thread_repo(db);
  holder::model::AiThread th1{
      .thread_id = "thread-1",
      .project_id = "proj-1",
      .card_id = std::nullopt,
      .title = "T1",
      .created_at = 1,
      .updated_at = 1,
  };
  thread_repo.create(th1);
  holder::model::AiThread th2{
      .thread_id = "thread-2",
      .project_id = "proj-2",
      .card_id = std::nullopt,
      .title = "T2",
      .created_at = 1,
      .updated_at = 1,
  };
  thread_repo.create(th2);

  holder::resource::ResourceRepo resource_repo(db);
  holder::model::Resource r1{
      .resource_id = "res-1",
      .project_id = "proj-1",
      .kind = "url",
      .uri = "https://example.com/1",
      .label = "R1",
      .desc = std::nullopt,
      .created_at = 1,
      .updated_at = 1,
  };
  resource_repo.add(r1);

  db.exec(
      "INSERT INTO ai_messages(message_id,thread_id,role,source,provider,model,content,created_at,deleted_at,prompt_hash,meta_json) "
      "VALUES('msg-proj2','thread-2','assistant','manual_paste',NULL,NULL,'body',101,NULL,NULL,NULL);"
  );

  SECTION("links card not found") {
    auto [status, payload] = call(
        http::verb::get,
        "/cards/ffffffff-ffff-4fff-8fff-ffffffffffff/links",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::not_found);
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("links method not allowed") {
    auto [status, payload] = call(
        http::verb::patch,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::method_not_allowed);
    REQUIRE(payload["error"]["code"] == "method_not_allowed");
  }

  SECTION("links missing to_card_id") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("links default to_type card") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        {{"to_card_id", "22222222-2222-4222-8222-222222222222"}}
    );
    REQUIRE(status == http::status::created);
    REQUIRE(payload["data"]["to_type"] == "card");
  }

  SECTION("links unsupported to_type") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        {{"to_card_id", "x"}, {"to_type", "banana"}}
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("links ai_message target missing") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        {{"to_card_id", "msg-missing"}, {"to_type", "ai_message"}}
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("links ai_message cross-project target") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        {{"to_card_id", "msg-proj2"}, {"to_type", "ai_message"}}
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("links ai_thread cross-project target") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        {{"to_card_id", "thread-2"}, {"to_type", "ai_thread"}}
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("links resource target success and delete-all") {
    auto [create_status, create_payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        {{"to_card_id", "res-1"}, {"to_type", "resource"}}
    );
    REQUIRE(create_status == http::status::created);
    REQUIRE(create_payload["data"]["to_type"] == "resource");

    auto [list_status, list_payload] = call(
        http::verb::get,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        nlohmann::json::object()
    );
    REQUIRE(list_status == http::status::ok);
    REQUIRE(list_payload["data"].is_array());
    REQUIRE(list_payload["data"].size() >= 1);

    auto [del_status, del_payload] = call(
        http::verb::delete_,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        nlohmann::json::object()
    );
    REQUIRE(del_status == http::status::ok);
    REQUIRE(del_payload["ok"] == true);
  }

  SECTION("backlinks card not found and method guard") {
    auto [not_found_status, not_found_payload] = call(
        http::verb::get,
        "/cards/ffffffff-ffff-4fff-8fff-ffffffffffff/backlinks",
        nlohmann::json::object()
    );
    REQUIRE(not_found_status == http::status::not_found);
    REQUIRE(not_found_payload["error"]["code"] == "not_found");

    auto [method_status, method_payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/backlinks",
        nlohmann::json::object()
    );
    REQUIRE(method_status == http::status::method_not_allowed);
    REQUIRE(method_payload["error"]["code"] == "method_not_allowed");
  }

  SECTION("backlinks include_deleted toggles filtering") {
    auto [create_status, create_payload] = call(
        http::verb::post,
        "/cards/22222222-2222-4222-8222-222222222222/links",
        {{"to_card_id", "11111111-1111-4111-8111-111111111111"}}
    );
    REQUIRE(create_status == http::status::created);
    REQUIRE(create_payload["ok"] == true);

    auto [del_status, del_payload] = call(
        http::verb::delete_,
        "/cards/22222222-2222-4222-8222-222222222222",
        nlohmann::json::object()
    );
    REQUIRE(del_status == http::status::ok);
    REQUIRE(del_payload["ok"] == true);

    auto [filtered_status, filtered_payload] = call(
        http::verb::get,
        "/cards/11111111-1111-4111-8111-111111111111/backlinks",
        nlohmann::json::object()
    );
    REQUIRE(filtered_status == http::status::ok);
    REQUIRE(filtered_payload["data"].is_array());
    REQUIRE(filtered_payload["data"].empty());

    const auto include_deleted_param_get = map_param_getter({{"include_deleted", "1"}});
    auto [included_status, included_payload] = call_with_param_get(
        http::verb::get,
        "/cards/11111111-1111-4111-8111-111111111111/backlinks",
        nlohmann::json::object(),
        include_deleted_param_get
    );
    REQUIRE(included_status == http::status::ok);
    REQUIRE(included_payload["data"].is_array());
    REQUIRE(included_payload["data"].size() == 1);
    REQUIRE(included_payload["data"][0]["from_card_id"] == "22222222-2222-4222-8222-222222222222");
  }
}

TEST_CASE("CardRoutes item get and patch validation branches", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "repo";
  holder::test::create_project(db, "proj-1", project_root.string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  auto call = [&](http::verb method, const std::string& path, const nlohmann::json& body) {
    auto req = make_request(method, path, body.dump());
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        path,
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };

  auto create_card = [&](const std::string& card_id, const std::string& title, int t) {
    auto [status, payload] = call(
        http::verb::post,
        "/cards",
        {
            {"card_id", card_id},
            {"project_id", "proj-1"},
            {"title", title},
            {"content", title},
            {"created_at", t},
            {"updated_at", t},
        }
    );
    REQUIRE(status == http::status::created);
    REQUIRE(payload["ok"] == true);
  };

  create_card("11111111-1111-4111-8111-111111111111", "A", 10);

  SECTION("get card not found") {
    auto [status, payload] = call(
        http::verb::get,
        "/cards/ffffffff-ffff-4fff-8fff-ffffffffffff",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::not_found);
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("get card content missing") {
    auto [created_status, created_payload] = call(
        http::verb::post,
        "/cards",
        {
            {"card_id", "22222222-2222-4222-8222-222222222222"},
            {"project_id", "proj-1"},
            {"title", "B"},
            {"content", "B"},
            {"created_at", 11},
            {"updated_at", 11},
        }
    );
    REQUIRE(created_status == http::status::created);
    const auto rel = created_payload["data"]["rel_path"].get<std::string>();
    std::filesystem::remove(project_root / rel);

    auto [status, payload] = call(
        http::verb::get,
        "/cards/22222222-2222-4222-8222-222222222222",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::not_found);
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("patch missing updated_at") {
    auto [status, payload] =
        call(http::verb::patch, "/cards/11111111-1111-4111-8111-111111111111", {{"content", "X"}});
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("patch no updatable fields") {
    auto [status, payload] = call(
        http::verb::patch,
        "/cards/11111111-1111-4111-8111-111111111111",
        {{"updated_at", 20}}
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("patch title without content") {
    auto [status, payload] = call(
        http::verb::patch,
        "/cards/11111111-1111-4111-8111-111111111111",
        {{"title", "New"}, {"updated_at", 21}}
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("post to item route returns not found") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::not_found);
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("unknown tail route returns not found") {
    auto [status, payload] = call(
        http::verb::get,
        "/cards/11111111-1111-4111-8111-111111111111/unknown",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::not_found);
    REQUIRE(payload["error"]["code"] == "not_found");
  }
}

TEST_CASE("CardRoutes additional uncovered branches", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", (dir / "repo1").string());
  holder::test::create_project(db, "proj-2", (dir / "repo2").string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto empty_param_get = [](const std::string&) {
    return std::string();
  };

  auto call = [&](http::verb method, const std::string& path, const nlohmann::json& body) {
    auto req = make_request(method, path, body.dump());
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        path,
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };
  auto call_with_params = [&](http::verb method,
                              const std::string& path,
                              const nlohmann::json& body,
                              const std::unordered_map<std::string, std::string>& params) {
    auto req = make_request(method, path, body.dump());
    http::response<http::string_body> res;
    const auto param_get = map_param_getter(params);
    const bool handled = holder::api::routes::handle_card_routes(
        path,
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };

  auto create_card = [&](const std::string& card_id,
                         const std::string& project_id,
                         const std::string& title,
                         int t,
                         const nlohmann::json& extra = nlohmann::json::object()) {
    nlohmann::json body{
        {"card_id", card_id},
        {"project_id", project_id},
        {"title", title},
        {"content", title},
        {"created_at", t},
        {"updated_at", t},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
      body[it.key()] = it.value();
    }
    auto [status, payload] = call(http::verb::post, "/cards", body);
    REQUIRE(status == http::status::created);
    REQUIRE(payload["ok"] == true);
  };

  create_card("11111111-1111-4111-8111-111111111111", "proj-1", "alpha", 10, {{"sort_key", 100.0}});
  create_card("22222222-2222-4222-8222-222222222222", "proj-1", "Alpha", 10, {{"sort_key", 100.0}});
  create_card("33333333-3333-4333-8333-333333333333", "proj-1", "alpha", 9, {{"sort_key", 100.0}});
  create_card("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "proj-2", "x", 5);

  SECTION("context accepts blank parent and explicit tree_default/count false") {
    auto [status, payload] = call_with_params(
        http::verb::get,
        "/cards/context",
        nlohmann::json::object(),
        {{"project_id", "proj-1"},
         {"parent_card_id", "   \t"},
         {"order", "tree_default"},
         {"count", "0"}}
    );
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"]["current_parent_card_id"].is_null());
  }

  SECTION("recent cards supports explicit tree_default order and count") {
    auto [status, payload] = call_with_params(
        http::verb::get,
        "/cards",
        nlohmann::json::object(),
        {{"project_id", "proj-1"},
         {"view", "recent"},
         {"order", "tree_default"},
         {"count", "1"},
         {"limit", "10"}}
    );
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["ok"] == true);
    REQUIRE(payload["data"].is_array());
    REQUIRE(payload["data"].size() >= 1);
    REQUIRE(payload["data"][0].contains("child_count"));
  }

  SECTION("tree list filters deleted by default and can include child_count") {
    auto [trash_status, trash_payload] = call(
        http::verb::delete_,
        "/cards/33333333-3333-4333-8333-333333333333",
        nlohmann::json::object()
    );
    REQUIRE(trash_status == http::status::ok);
    REQUIRE(trash_payload["ok"] == true);

    auto [status, payload] = call_with_params(
        http::verb::get,
        "/cards",
        nlohmann::json::object(),
        {{"project_id", "proj-1"}, {"view", "tree"}, {"count", "1"}}
    );
    REQUIRE(status == http::status::ok);
    for (const auto& row : payload["data"]) {
      REQUIRE(row["card_id"] != "33333333-3333-4333-8333-333333333333");
      REQUIRE(row.contains("child_count"));
    }
  }

  SECTION("post /cards validates required fields and optional rel_path/sort_key") {
    auto [bad_status, bad_payload] =
        call(http::verb::post, "/cards", {{"project_id", "proj-1"}, {"title", "missing content"}});
    REQUIRE(bad_status == http::status::bad_request);
    REQUIRE(bad_payload["error"]["code"] == "bad_request");

    auto [ok_status, ok_payload] = call(
        http::verb::post,
        "/cards",
        {{"card_id", "44444444-4444-4444-8444-444444444444"},
         {"project_id", "proj-1"},
         {"title", "manual"},
         {"content", "manual"},
         {"created_at", 20},
         {"updated_at", 20},
         {"sort_key", 555.0},
         {"rel_path", "cards/custom.md"}}
    );
    REQUIRE(ok_status == http::status::bad_request);
    REQUIRE(ok_payload["error"]["code"] == "bad_request");
  }

  SECTION("move to_start and to_end no-op with no siblings") {
    create_card(
        "55555555-5555-4555-8555-555555555555",
        "proj-1",
        "lonely",
        30,
        {{"parent_card_id", "11111111-1111-4111-8111-111111111111"}}
    );

    auto [start_status, start_payload] = call(
        http::verb::post,
        "/cards/55555555-5555-4555-8555-555555555555/move",
        {{"project_id", "proj-1"},
         {"intent", "to_start"},
         {"parent_card_id", "11111111-1111-4111-8111-111111111111"}}
    );
    REQUIRE(start_status == http::status::ok);
    REQUIRE(start_payload["ok"] == true);

    auto [end_status, end_payload] = call(
        http::verb::post,
        "/cards/55555555-5555-4555-8555-555555555555/move",
        {{"project_id", "proj-1"},
         {"intent", "to_end"},
         {"parent_card_id", "11111111-1111-4111-8111-111111111111"}}
    );
    REQUIRE(end_status == http::status::ok);
    REQUIRE(end_payload["ok"] == true);
  }

  SECTION("move up_level from missing parent falls back to root") {
    create_card("66666666-6666-4666-8666-666666666666", "proj-1", "parent", 40);
    create_card(
        "77777777-7777-4777-8777-777777777777",
        "proj-1",
        "child",
        41,
        {{"parent_card_id", "66666666-6666-4666-8666-666666666666"}}
    );
    auto [trash_status, trash_payload] = call(
        http::verb::delete_,
        "/cards/66666666-6666-4666-8666-666666666666",
        nlohmann::json::object()
    );
    REQUIRE(trash_status == http::status::ok);
    REQUIRE(trash_payload["ok"] == true);

    auto [move_status, move_payload] = call(
        http::verb::post,
        "/cards/77777777-7777-4777-8777-777777777777/move",
        {{"project_id", "proj-1"}, {"intent", "up_level"}}
    );
    REQUIRE(move_status == http::status::ok);
    REQUIRE(move_payload["ok"] == true);
    REQUIRE(move_payload["data"]["parent_card_id"].is_null());
  }

  SECTION("move catches bad request from non-string intent") {
    auto [status, payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/move",
        {{"project_id", "proj-1"}, {"intent", 123}}
    );
    REQUIRE(status == http::status::bad_request);
    REQUIRE(payload["error"]["code"] == "bad_request");
  }

  SECTION("backlinks filter unknown source id when include_deleted is false") {
    holder::card::LinkRepo link_repo(db);
    holder::model::CardLink ghost{
        .project_id = "proj-1",
        .from_card_id = "ghost-source",
        .to_card_id = "11111111-1111-4111-8111-111111111111",
        .to_type = "card",
        .kind = "ref",
        .label = std::nullopt,
        .created_at = 1,
    };
    link_repo.upsert_links("proj-1", "ghost-source", {ghost});

    auto [status, payload] = call(
        http::verb::get,
        "/cards/11111111-1111-4111-8111-111111111111/backlinks",
        nlohmann::json::object()
    );
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["data"].is_array());
    REQUIRE(payload["data"].empty());
  }

  SECTION("links delete parses to_type and catches invalid json") {
    auto [create_status, create_payload] = call(
        http::verb::post,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        {{"to_card_id", "22222222-2222-4222-8222-222222222222"}, {"to_type", "card"}}
    );
    REQUIRE(create_status == http::status::created);
    REQUIRE(create_payload["ok"] == true);

    auto req = make_request(
        http::verb::delete_,
        "/cards/11111111-1111-4111-8111-111111111111/links",
        R"({"to_card_id":"22222222-2222-4222-8222-222222222222","to_type":"card"})"
    );
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        "/cards/11111111-1111-4111-8111-111111111111/links",
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::ok);

    auto bad_req =
        make_request(http::verb::post, "/cards/11111111-1111-4111-8111-111111111111/links", "{");
    http::response<http::string_body> bad_res;
    const bool bad_handled = holder::api::routes::handle_card_routes(
        "/cards/11111111-1111-4111-8111-111111111111/links",
        bad_req,
        bad_res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(bad_handled);
    REQUIRE(bad_res.result() == http::status::bad_request);
  }

  SECTION("restore method guard and missing card error") {
    auto [guard_status, guard_payload] = call(
        http::verb::get,
        "/cards/11111111-1111-4111-8111-111111111111/restore",
        nlohmann::json::object()
    );
    REQUIRE(guard_status == http::status::method_not_allowed);
    REQUIRE(guard_payload["error"]["code"] == "method_not_allowed");

    auto [missing_status, missing_payload] = call(
        http::verb::post,
        "/cards/ffffffff-ffff-4fff-8fff-ffffffffffff/restore",
        nlohmann::json::object()
    );
    REQUIRE(missing_status == http::status::bad_request);
    REQUIRE(missing_payload["error"]["code"] == "bad_request");
  }

  SECTION("item routes cover no-store, empty id, deleted and missing delete") {
    auto req = make_request(http::verb::get, "/cards/11111111-1111-4111-8111-111111111111");
    http::response<http::string_body> no_store_res;
    const bool no_store_handled = holder::api::routes::handle_card_routes(
        "/cards/11111111-1111-4111-8111-111111111111",
        req,
        no_store_res,
        db,
        nullptr,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(no_store_handled);
    REQUIRE(no_store_res.result() == http::status::not_implemented);

    auto [empty_status, empty_payload] = call(http::verb::get, "/cards/", nlohmann::json::object());
    REQUIRE(empty_status == http::status::not_found);
    REQUIRE(empty_payload["error"]["code"] == "not_found");

    auto [trash_status, trash_payload] = call(
        http::verb::delete_,
        "/cards/22222222-2222-4222-8222-222222222222",
        nlohmann::json::object()
    );
    REQUIRE(trash_status == http::status::ok);
    REQUIRE(trash_payload["ok"] == true);

    auto [deleted_get_status, deleted_get_payload] = call(
        http::verb::get,
        "/cards/22222222-2222-4222-8222-222222222222",
        nlohmann::json::object()
    );
    REQUIRE(deleted_get_status == http::status::not_found);
    REQUIRE(deleted_get_payload["error"]["code"] == "not_found");

    auto [delete_missing_status, delete_missing_payload] = call(
        http::verb::delete_,
        "/cards/ffffffff-ffff-4fff-8fff-ffffffffffff",
        nlohmann::json::object()
    );
    REQUIRE(delete_missing_status == http::status::bad_request);
    REQUIRE(delete_missing_payload["error"]["code"] == "bad_request");
  }
}

TEST_CASE("CardRoutes residual branch coverage", "[card-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", (dir / "repo1").string());
  holder::test::create_project(db, "proj-2", (dir / "repo2").string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto empty_param_get = [](const std::string&) {
    return std::string();
  };

  auto call = [&](http::verb method, const std::string& path, const nlohmann::json& body) {
    auto req = make_request(method, path, body.dump());
    http::response<http::string_body> res;
    const bool handled = holder::api::routes::handle_card_routes(
        path,
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };
  auto call_with_params = [&](http::verb method,
                              const std::string& path,
                              const nlohmann::json& body,
                              const std::unordered_map<std::string, std::string>& params) {
    auto req = make_request(method, path, body.dump());
    http::response<http::string_body> res;
    const auto param_get = map_param_getter(params);
    const bool handled = holder::api::routes::handle_card_routes(
        path,
        req,
        res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    return std::make_pair(res.result(), nlohmann::json::parse(res.body()));
  };
  auto create_card = [&](const std::string& card_id,
                         const std::string& project_id,
                         const std::string& title,
                         int t,
                         const nlohmann::json& extra = nlohmann::json::object()) {
    nlohmann::json body{
        {"card_id", card_id},
        {"project_id", project_id},
        {"title", title},
        {"content", title},
        {"created_at", t},
        {"updated_at", t},
    };
    for (auto it = extra.begin(); it != extra.end(); ++it) {
      body[it.key()] = it.value();
    }
    auto [status, payload] = call(http::verb::post, "/cards", body);
    REQUIRE(status == http::status::created);
    REQUIRE(payload["ok"] == true);
  };

  create_card("00000000-0000-4000-8000-000000000001", "proj-1", "A", 10, {{"sort_key", 100.0}});
  create_card("00000000-0000-4000-8000-000000000002", "proj-1", "B", 10, {{"sort_key", 100.0}});
  create_card("00000000-0000-4000-8000-000000000003", "proj-1", "C", 11, {{"sort_key", 100.00001}});
  create_card("00000000-0000-4000-8000-000000000004", "proj-2", "X", 20);

  SECTION("context returns early on invalid count and catches db failure") {
    auto [bad_count_status, bad_count_payload] = call_with_params(
        http::verb::get,
        "/cards/context",
        nlohmann::json::object(),
        {{"project_id", "proj-1"}, {"count", "maybe"}}
    );
    REQUIRE(bad_count_status == http::status::bad_request);
    REQUIRE(bad_count_payload["error"]["code"] == "bad_request");

    holder::platform::Db unopened;
    auto req = make_request(http::verb::get, "/cards/context");
    http::response<http::string_body> res;
    const auto param_get = map_param_getter({{"project_id", "proj-1"}});
    const bool handled = holder::api::routes::handle_card_routes(
        "/cards/context",
        req,
        res,
        unopened,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("context skips deleted level cards and broken breadcrumb chain") {
    auto [trash_status, trash_payload] = call(
        http::verb::delete_,
        "/cards/00000000-0000-4000-8000-000000000002",
        nlohmann::json::object()
    );
    REQUIRE(trash_status == http::status::ok);
    REQUIRE(trash_payload["ok"] == true);

    // Create a valid chain then delete the ancestor so breadcrumb traversal breaks.
    create_card("00000000-0000-4000-8000-000000000099", "proj-1", "BrokenParent", 12);
    create_card(
        "00000000-0000-4000-8000-000000000100",
        "proj-1",
        "ChildBroken",
        13,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000099"}}
    );
    auto [del_parent_status, del_parent_payload] = call(
        http::verb::delete_,
        "/cards/00000000-0000-4000-8000-000000000099",
        nlohmann::json::object()
    );
    REQUIRE(del_parent_status == http::status::ok);
    REQUIRE(del_parent_payload["ok"] == true);

    auto [status, payload] = call_with_params(
        http::verb::get,
        "/cards/context",
        nlohmann::json::object(),
        {{"project_id", "proj-1"},
         {"parent_card_id", "00000000-0000-4000-8000-000000000100"},
         {"count", "true"}}
    );
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["ok"] == true);
    for (const auto& row : payload["data"]["cards"]) {
      REQUIRE(row["card_id"] != "00000000-0000-4000-8000-000000000002");
    }
  }

  SECTION("recent updated_desc tie-breakers and cards get db catch path") {
    auto [s1, p1] = call_with_params(
        http::verb::get,
        "/cards",
        nlohmann::json::object(),
        {{"project_id", "proj-1"},
         {"view", "recent"},
         {"order", "updated_desc"},
         {"count", "0"},
         {"limit", "10"}}
    );
    REQUIRE(s1 == http::status::ok);
    REQUIRE(p1["ok"] == true);

    holder::platform::Db unopened;
    auto req = make_request(http::verb::get, "/cards");
    http::response<http::string_body> res;
    const auto param_get = map_param_getter({{"project_id", "proj-1"}, {"view", "tree"}});
    const bool handled = holder::api::routes::handle_card_routes(
        "/cards",
        req,
        res,
        unopened,
        &card_store,
        &fts,
        uuid_v4,
        param_get
    );
    REQUIRE(handled);
    REQUIRE(res.result() == http::status::internal_server_error);
  }

  SECTION(
      "move covers null parent parse, deleted sibling skip, dense sort fallback and cross-project target"
  ) {
    create_card("00000000-0000-4000-8000-000000000010", "proj-1", "P", 30);
    create_card(
        "00000000-0000-4000-8000-000000000011",
        "proj-1",
        "S1",
        31,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000010"}, {"sort_key", 10.0}}
    );
    create_card(
        "00000000-0000-4000-8000-000000000012",
        "proj-1",
        "S2",
        31,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000010"}, {"sort_key", 10.00001}}
    );
    create_card(
        "00000000-0000-4000-8000-000000000013",
        "proj-1",
        "S3",
        31,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000010"}, {"sort_key", 10.00002}}
    );
    create_card(
        "00000000-0000-4000-8000-000000000014",
        "proj-1",
        "S0",
        31,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000010"}, {"sort_key", 10.0}}
    );
    auto [trash_status, _trash_payload] = call(
        http::verb::delete_,
        "/cards/00000000-0000-4000-8000-000000000013",
        nlohmann::json::object()
    );
    REQUIRE(trash_status == http::status::ok);

    // Dense sort-gap fallback and sibling comparator branches.
    auto [before_status, before_payload] = call(
        http::verb::post,
        "/cards/00000000-0000-4000-8000-000000000012/move",
        {{"project_id", "proj-1"},
         {"intent", "before"},
         {"target_card_id", "00000000-0000-4000-8000-000000000011"}}
    );
    REQUIRE(before_status == http::status::ok);
    REQUIRE(before_payload["ok"] == true);

    // parent_card_id null exercises normalize_parent_id(json null) branch.
    auto [to_end_status, to_end_payload] = call(
        http::verb::post,
        "/cards/00000000-0000-4000-8000-000000000011/move",
        {{"project_id", "proj-1"}, {"intent", "to_end"}, {"parent_card_id", nullptr}}
    );
    REQUIRE(to_end_status == http::status::ok);
    REQUIRE(to_end_payload["ok"] == true);

    auto [left_status, left_payload] = call(
        http::verb::post,
        "/cards/00000000-0000-4000-8000-000000000012/move",
        {{"project_id", "proj-1"}, {"intent", "left"}}
    );
    REQUIRE(left_status == http::status::ok);
    REQUIRE(left_payload["ok"] == true);
  }

  SECTION("move right ordering uses updated_at and title tie-breakers for equal sort_key") {
    create_card("00000000-0000-4000-8000-000000000030", "proj-1", "P2", 50);
    create_card(
        "00000000-0000-4000-8000-000000000031",
        "proj-1",
        "M",
        10,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000030"}, {"sort_key", 50.0}}
    );
    create_card(
        "00000000-0000-4000-8000-000000000032",
        "proj-1",
        "Z",
        20,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000030"}, {"sort_key", 50.0}}
    );
    create_card(
        "00000000-0000-4000-8000-000000000033",
        "proj-1",
        "A",
        20,
        {{"parent_card_id", "00000000-0000-4000-8000-000000000030"}, {"sort_key", 50.0}}
    );

    auto [status, payload] = call(
        http::verb::post,
        "/cards/00000000-0000-4000-8000-000000000033/move",
        {{"project_id", "proj-1"}, {"intent", "right"}}
    );
    REQUIRE(status == http::status::ok);
    REQUIRE(payload["ok"] == true);
  }

  SECTION("backlinks and item get catch branches plus final route-not-found branch") {
    // Create a card in db/card_store and then force route db failure for backlinks.
    create_card("00000000-0000-4000-8000-000000000020", "proj-1", "Back", 40);

    holder::platform::Db unopened;
    auto backlinks_req =
        make_request(http::verb::get, "/cards/00000000-0000-4000-8000-000000000020/backlinks");
    http::response<http::string_body> backlinks_res;
    const bool backlinks_handled = holder::api::routes::handle_card_routes(
        "/cards/00000000-0000-4000-8000-000000000020/backlinks",
        backlinks_req,
        backlinks_res,
        unopened,
        &card_store,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(backlinks_handled);
    REQUIRE(backlinks_res.result() == http::status::bad_request);

    // Force item get exception by breaking rel_path invariant.
    db.exec(
        "UPDATE cards SET rel_path='cards/bad-path.md' WHERE card_id='00000000-0000-4000-8000-000000000020';"
    );
    auto get_req = make_request(http::verb::get, "/cards/00000000-0000-4000-8000-000000000020");
    http::response<http::string_body> get_res;
    const bool get_handled = holder::api::routes::handle_card_routes(
        "/cards/00000000-0000-4000-8000-000000000020",
        get_req,
        get_res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(get_handled);
    REQUIRE(get_res.result() == http::status::internal_server_error);

    // Method not handled by any item branch -> final not_found branch.
    auto put_req = make_request(http::verb::put, "/cards/00000000-0000-4000-8000-000000000020");
    http::response<http::string_body> put_res;
    const bool put_handled = holder::api::routes::handle_card_routes(
        "/cards/00000000-0000-4000-8000-000000000020",
        put_req,
        put_res,
        db,
        &card_store,
        &fts,
        uuid_v4,
        empty_param_get
    );
    REQUIRE(put_handled);
    REQUIRE(put_res.result() == http::status::not_found);
  }
}
