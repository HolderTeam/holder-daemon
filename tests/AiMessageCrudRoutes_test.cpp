#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/messages/AiMessageCrudRoutes.h"
#include "ai/AiMessageRepo.h"
#include "ai/AiThreadRepo.h"
#include "http_test_helpers.h"
#include "model/AiThread.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

namespace {

namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method,
                                               const std::string& target,
                                               const std::string& body = "") {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body;
    req.prepare_payload();
  }
  return req;
}

void seed_project_and_thread(holder::platform::Db& db, const std::filesystem::path& root) {
  holder::test::create_project(db, "proj-1", root.string());
  holder::model::AiThread thread;
  thread.thread_id = "thread-1";
  thread.project_id = "proj-1";
  thread.title = "Thread";
  thread.created_at = 1;
  thread.updated_at = 1;
  holder::ai::AiThreadRepo(db).create(thread);
}

} // namespace

TEST_CASE("AiMessageCrudRoutes collection branches", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  seed_project_and_thread(db, dir / "project_repo");
  holder::index::FtsIndexer fts(db);

  SECTION("list filters deleted unless include_deleted=1") {
    const auto no_query = [](const std::string&) -> std::string { return {}; };
    auto post1 = make_request(http::verb::post,
                              "/ai/messages",
                              R"({"message_id":"msg-0001","thread_id":"thread-1","role":"user","source":"manual","content":"a","created_at":10})");
    http::response<http::string_body> post1_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", post1, post1_res, db, &fts, [] { return std::string("x"); }, no_query));
    REQUIRE(post1_res.result() == http::status::created);

    auto post2 = make_request(http::verb::post,
                              "/ai/messages",
                              R"({"message_id":"msg-0002","thread_id":"thread-1","role":"user","source":"manual","content":"b","created_at":11})");
    http::response<http::string_body> post2_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", post2, post2_res, db, &fts, [] { return std::string("y"); }, no_query));
    REQUIRE(post2_res.result() == http::status::created);

    holder::ai::AiMessageRepo(db, &fts).trash("msg-0002", 99);

    auto list_req = make_request(http::verb::get, "/ai/messages");
    http::response<http::string_body> list_res;
    const auto thread_only = [](const std::string& key) -> std::string {
      if (key == "thread_id") return "thread-1";
      return {};
    };
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", list_req, list_res, db, &fts, [] { return std::string("z"); }, thread_only));
    REQUIRE(list_res.result() == http::status::ok);
    const auto listed = nlohmann::json::parse(list_res.body());
    REQUIRE(listed["data"].is_array());
    REQUIRE(listed["data"].size() == 1);
    REQUIRE(listed["data"][0]["message_id"] == "msg-0001");

    http::response<http::string_body> list_all_res;
    const auto include_deleted = [](const std::string& key) -> std::string {
      if (key == "thread_id") return "thread-1";
      if (key == "include_deleted") return "1";
      return {};
    };
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", list_req, list_all_res, db, &fts, [] { return std::string("z"); }, include_deleted));
    REQUIRE(list_all_res.result() == http::status::ok);
    const auto listed_all = nlohmann::json::parse(list_all_res.body());
    REQUIRE(listed_all["data"].size() == 2);
  }

  SECTION("list catch maps repo errors to bad_request") {
    db.exec("DROP TABLE ai_messages;");
    auto req = make_request(http::verb::get, "/ai/messages");
    http::response<http::string_body> res;
    const auto q = [](const std::string& key) -> std::string {
      if (key == "thread_id") return "thread-1";
      return {};
    };
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", req, res, db, &fts, [] { return std::string("u"); }, q));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("post missing required fields") {
    auto req = make_request(http::verb::post, "/ai/messages", R"({"thread_id":"thread-1"})");
    http::response<http::string_body> res;
    const auto no_query = [](const std::string&) -> std::string { return {}; };
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", req, res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("post sets optional fields and created_at fallback") {
    auto req = make_request(http::verb::post,
                            "/ai/messages",
                            R"({"thread_id":"thread-1","role":"assistant","source":"runner","content":"hello","provider":"prov","model":"mod","prompt_hash":"ph","meta_json":"{}","created_at":0})");
    http::response<http::string_body> res;
    const auto no_query = [](const std::string&) -> std::string { return {}; };
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", req, res, db, &fts, [] { return std::string("generated-m"); }, no_query));
    REQUIRE(res.result() == http::status::created);
    const auto body = nlohmann::json::parse(res.body());
    const auto id = body["data"]["message_id"].get<std::string>();

    auto get_req = make_request(http::verb::get, "/ai/messages/" + id);
    http::response<http::string_body> get_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/" + id, get_req, get_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(get_res.result() == http::status::ok);
    const auto got = nlohmann::json::parse(get_res.body());
    REQUIRE(got["data"]["provider"] == "prov");
    REQUIRE(got["data"]["model"] == "mod");
    REQUIRE(got["data"]["prompt_hash"] == "ph");
    REQUIRE(got["data"]["meta_json"] == "{}");
    REQUIRE(got["data"]["created_at"].get<long long>() > 0);
  }

  SECTION("post conflict and generic catches") {
    const auto no_query = [](const std::string&) -> std::string { return {}; };
    auto req_conflict = make_request(http::verb::post,
                                     "/ai/messages",
                                     R"({"thread_id":"thread-1","role":"assistant","source":"runner","content":"hello"})");
    http::response<http::string_body> conflict_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages",
        req_conflict,
        conflict_res,
        db,
        &fts,
        []() -> std::string { throw std::runtime_error("conflict: synthetic"); },
        no_query));
    REQUIRE(conflict_res.result() == http::status::conflict);

    http::response<http::string_body> bad_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages", make_request(http::verb::post, "/ai/messages", "not-json"), bad_res, db, &fts, [] {
          return std::string("u");
        }, no_query));
    REQUIRE(bad_res.result() == http::status::bad_request);
  }
}

TEST_CASE("AiMessageCrudRoutes item and restore branches", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  seed_project_and_thread(db, dir / "project_repo");
  holder::index::FtsIndexer fts(db);
  const auto no_query = [](const std::string&) -> std::string { return {}; };

  auto create_req = make_request(http::verb::post,
                                 "/ai/messages",
                                 R"({"message_id":"msg-1","thread_id":"thread-1","role":"user","source":"manual","content":"c","provider":"p","model":"m","prompt_hash":"h","meta_json":"{}","created_at":10})");
  http::response<http::string_body> create_res;
  REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
      "/ai/messages", create_req, create_res, db, &fts, [] { return std::string("u"); }, no_query));
  REQUIRE(create_res.result() == http::status::created);

  SECTION("route guards and fallbacks") {
    http::response<http::string_body> res;
    REQUIRE_FALSE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messagez/msg-1", make_request(http::verb::get, "/ai/messagez/msg-1"), res, db, &fts, [] {
          return std::string("u");
        }, no_query));

    REQUIRE_FALSE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/", make_request(http::verb::get, "/ai/messages/"), res, db, &fts, [] {
          return std::string("u");
        }, no_query));

    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages//restore",
        make_request(http::verb::post, "/ai/messages//restore"),
        res,
        db,
        &fts,
        [] { return std::string("u"); },
        no_query));
    REQUIRE(res.result() == http::status::not_found);

    REQUIRE_FALSE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1/other",
        make_request(http::verb::get, "/ai/messages/msg-1/other"),
        res,
        db,
        &fts,
        [] { return std::string("u"); },
        no_query));
  }

  SECTION("restore method and catch branches") {
    auto wrong_method = make_request(http::verb::get, "/ai/messages/msg-1/restore");
    http::response<http::string_body> wrong_method_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1/restore", wrong_method, wrong_method_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(wrong_method_res.result() == http::status::method_not_allowed);

    auto missing_restore = make_request(http::verb::post, "/ai/messages/missing/restore");
    http::response<http::string_body> missing_restore_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/missing/restore",
        missing_restore,
        missing_restore_res,
        db,
        &fts,
        [] { return std::string("u"); },
        no_query));
    REQUIRE(missing_restore_res.result() == http::status::bad_request);
  }

  SECTION("delete catch branch") {
    db.exec("DROP TABLE ai_messages;");
    auto req = make_request(http::verb::delete_, "/ai/messages/msg-1");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1", req, res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("patch not-found, deleted, optional clears, and catch") {
    auto patch_missing = make_request(http::verb::patch, "/ai/messages/missing", R"({"content":"x"})");
    http::response<http::string_body> missing_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/missing", patch_missing, missing_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(missing_res.result() == http::status::not_found);

    holder::ai::AiMessageRepo(db, &fts).trash("msg-1", 123);
    auto patch_deleted = make_request(http::verb::patch, "/ai/messages/msg-1", R"({"content":"x"})");
    http::response<http::string_body> deleted_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1", patch_deleted, deleted_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(deleted_res.result() == http::status::bad_request);

    holder::ai::AiMessageRepo(db, &fts).restore("msg-1");
    auto patch_clear = make_request(http::verb::patch,
                                    "/ai/messages/msg-1",
                                    R"({"role":"assistant","source":"runner","provider":null,"model":null,"prompt_hash":null,"meta_json":null})");
    http::response<http::string_body> patch_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1", patch_clear, patch_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(patch_res.result() == http::status::ok);

    auto get_req = make_request(http::verb::get, "/ai/messages/msg-1");
    http::response<http::string_body> get_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1", get_req, get_res, db, &fts, [] { return std::string("u"); }, no_query));
    const auto got = nlohmann::json::parse(get_res.body());
    REQUIRE(got["data"]["role"] == "assistant");
    REQUIRE(got["data"]["source"] == "runner");
    REQUIRE(got["data"]["provider"].is_null());
    REQUIRE(got["data"]["model"].is_null());
    REQUIRE(got["data"]["prompt_hash"].is_null());
    REQUIRE(got["data"]["meta_json"].is_null());

    auto bad_json = make_request(http::verb::patch, "/ai/messages/msg-1", "{");
    http::response<http::string_body> bad_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1", bad_json, bad_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(bad_res.result() == http::status::bad_request);
  }

  SECTION("item post route and item get catch") {
    auto post_req = make_request(http::verb::post, "/ai/messages/msg-1");
    http::response<http::string_body> post_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1", post_req, post_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(post_res.result() == http::status::not_found);

    db.exec("DROP TABLE ai_messages;");
    auto get_req = make_request(http::verb::get, "/ai/messages/msg-1");
    http::response<http::string_body> get_res;
    REQUIRE(holder::api::routes::ai::messages::handle_ai_message_crud_routes(
        "/ai/messages/msg-1", get_req, get_res, db, &fts, [] { return std::string("u"); }, no_query));
    REQUIRE(get_res.result() == http::status::bad_request);
  }
}
