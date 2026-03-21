#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "ai/NudgeService.h"
#include "api/routes/ai/AiDispatch.h"
#include "http_test_helpers.h"
#include "privacy/SecretStore.h"

#include <boost/asio/ip/tcp.hpp>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

void create_card_fixture(holder::platform::Db& db,
                         const std::string& project_id,
                         const std::string& card_id) {
  db.exec(
      "INSERT INTO cards(card_id, project_id, title, rel_path, created_at, updated_at) "
      "VALUES('" + card_id + "', '" + project_id + "', 'Fixture', 'cards/" + card_id + ".md', 1, 1);");
}

} // namespace

TEST_CASE("AiDispatch returns unhandled for malformed and unknown routes", "[ai][dispatch]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;
  holder::ai::NudgeService nudge_service(db);
  auto secret_store = holder::privacy::make_default_secret_store(dir);

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  SECTION("path without leading slash") {
    auto req = make_request(http::verb::get, "ai/status");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "ai/status", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("root-only slash path") {
    auto req = make_request(http::verb::get, "/");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("status family unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/status/unknown");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/status/unknown", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("providers unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/providers/unknown");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/providers/unknown", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("nudges unmatched method/path combo") {
    auto req = make_request(http::verb::get, "/ai/nudges/evaluate");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/nudges/evaluate", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("runs unmatched method/path combo") {
    auto req = make_request(http::verb::delete_, "/ai/runs");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/runs", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("runner unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/runner/unknown");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/runner/unknown", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("threads unmatched subroute") {
    auto req = make_request(http::verb::delete_, "/ai/threads");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/threads", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }

  SECTION("messages unmatched subroute") {
    auto req = make_request(http::verb::get, "/ai/messages/msg-1/other");
    const auto out = holder::api::routes::ai::dispatch_ai_routes(
        "/ai/messages/msg-1/other", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
    REQUIRE_FALSE(out.handled);
    REQUIRE_FALSE(out.streamed);
  }
}

TEST_CASE("AiDispatch handles nudge evaluation routes", "[ai][dispatch]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1");
  create_card_fixture(db, "proj-1", "card-1");

  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;
  holder::ai::NudgeService nudge_service(db);
  auto secret_store = holder::privacy::make_default_secret_store(dir);

  const auto uuid_v4 = []() { return std::string("generated-id"); };
  const auto param_get = [](const std::string&) { return std::string(); };

  auto req = make_request(http::verb::post, "/ai/nudges/evaluate");
  req.body() = R"({
    "kind":"card.title_only",
    "project_id":"proj-1",
    "card_id":"card-1",
    "created_at":123,
    "facts":{
      "title":"Frog",
      "body_empty":true,
      "doc_chars":12,
      "body_chars":0
    }
  })";
  req.prepare_payload();

  const auto out = holder::api::routes::ai::dispatch_ai_routes(
      "/ai/nudges/evaluate", req, res, socket, db, nullptr, &nudge_service, secret_store.get(), nullptr, uuid_v4, param_get);
  REQUIRE(out.handled);
  REQUIRE_FALSE(out.streamed);
  REQUIRE(res.result() == http::status::ok);
}
