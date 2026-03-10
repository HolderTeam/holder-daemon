#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/ai/threads/AiThreadItemRoutes.h"
#include "http_test_helpers.h"

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

} // namespace

TEST_CASE("AiThreadItemRoutes covers guard and error branches", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  SECTION("non-matching path returns false") {
    auto req = make_request(http::verb::get, "/ai/threadz/t1");
    http::response<http::string_body> res;
    REQUIRE_FALSE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threadz/t1", req, res, db));
  }

  SECTION("empty item id returns route not found payload") {
    auto req = make_request(http::verb::get, "/ai/threads/");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/", req, res, db));
    REQUIRE(res.result() == http::status::not_found);
  }

  SECTION("get missing thread returns not_found") {
    auto req = make_request(http::verb::get, "/ai/threads/missing");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/missing", req, res, db));
    REQUIRE(res.result() == http::status::not_found);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["code"] == "not_found");
  }

  SECTION("get catches repo errors as bad_request") {
    db.exec("DROP TABLE ai_threads;");
    auto req = make_request(http::verb::get, "/ai/threads/t1");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/t1", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
  }
}

TEST_CASE("AiThreadItemRoutes patch/delete branches and errors", "[http]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, card_id, created_at, updated_at) "
          "VALUES('t1', 'proj-1', 'T', NULL, 1, 1);");

  SECTION("patch requires updated_at") {
    auto req = make_request(http::verb::patch, "/ai/threads/t1", R"({"title":"X"})");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/t1", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Missing updated_at.");
  }

  SECTION("patch without title touches updated_at") {
    auto req = make_request(http::verb::patch, "/ai/threads/t1", R"({"updated_at":99})");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/t1", req, res, db));
    REQUIRE(res.result() == http::status::ok);

    auto get_req = make_request(http::verb::get, "/ai/threads/t1");
    http::response<http::string_body> get_res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/t1", get_req, get_res, db));
    REQUIRE(get_res.result() == http::status::ok);
    const auto payload = nlohmann::json::parse(get_res.body());
    REQUIRE(payload["data"]["updated_at"] == 99);
  }

  SECTION("patch invalid json hits catch") {
    auto req = make_request(http::verb::patch, "/ai/threads/t1", "nope");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/t1", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("delete catches repo errors") {
    db.exec("DROP TABLE ai_threads;");
    auto req = make_request(http::verb::delete_, "/ai/threads/t1");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/t1", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("unsupported item method returns not_found route payload") {
    auto req = make_request(http::verb::post, "/ai/threads/t1", R"({"title":"ignored"})");
    http::response<http::string_body> res;
    REQUIRE(holder::api::routes::ai::threads::handle_ai_thread_item_routes(
        "/ai/threads/t1", req, res, db));
    REQUIRE(res.result() == http::status::not_found);
  }
}
