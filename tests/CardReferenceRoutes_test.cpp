#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/AuthenticatedRoutes.h"
#include "api/routes/CardReferenceRoutes.h"
#include "card/CardRepo.h"
#include "http_test_helpers.h"
#include "model/Card.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

namespace http = boost::beast::http;

http::request<http::string_body> make_request(
    http::verb method,
    const std::string& path,
    const std::string& body = ""
) {
  http::request<http::string_body> req{method, path, 11};
  req.body() = body;
  req.prepare_payload();
  return req;
}

http::response<http::string_body> resolve(holder::platform::Db& db, const nlohmann::json& body) {
  auto req = make_request(http::verb::post, "/card-references/resolve", body.dump());
  http::response<http::string_body> res;
  REQUIRE(
      holder::api::routes::handle_card_reference_routes("/card-references/resolve", req, res, db)
  );
  return res;
}

void create_card(
    holder::card::CardRepo& cards,
    const std::string& card_id,
    const std::string& title,
    const std::optional<long long>& deleted_at = std::nullopt
) {
  holder::model::Card card;
  card.card_id = card_id;
  card.project_id = "proj-1";
  card.title = title;
  card.rel_path = "cards/" + card_id + ".md";
  card.created_at = 1;
  card.updated_at = 1;
  card.deleted_at = deleted_at;
  cards.create(card);
}

} // namespace

TEST_CASE("CardReferenceRoutes only handles its POST endpoint", "[card-reference-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  http::response<http::string_body> res;

  auto other_path = make_request(http::verb::post, "/cards", "{}");
  REQUIRE_FALSE(holder::api::routes::handle_card_reference_routes("/cards", other_path, res, db));

  auto wrong_method = make_request(http::verb::get, "/card-references/resolve");
  REQUIRE_FALSE(holder::api::routes::handle_card_reference_routes(
      "/card-references/resolve",
      wrong_method,
      res,
      db
  ));
}

TEST_CASE("CardReferenceRoutes validates its request", "[card-reference-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  auto call_raw = [&](const std::string& body) {
    auto req = make_request(http::verb::post, "/card-references/resolve", body);
    http::response<http::string_body> res;
    REQUIRE(
        holder::api::routes::handle_card_reference_routes("/card-references/resolve", req, res, db)
    );
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["ok"] == false);
    REQUIRE(payload["error"]["code"] == "bad_request");
  };

  SECTION("invalid JSON") { call_raw("{"); }
  SECTION("missing field") { call_raw(R"({"project_id":"proj-1","reference":"12345678"})"); }
  SECTION("wrong field type") {
    call_raw(R"({"project_id":"proj-1","reference":7,"scope":"live"})");
  }
  SECTION("invalid scope") {
    call_raw(R"({"project_id":"proj-1","reference":"12345678","scope":"all"})");
  }
  SECTION("empty project") {
    call_raw(R"({"project_id":"","reference":"12345678","scope":"live"})");
  }
  SECTION("empty reference") {
    call_raw(R"({"project_id":"proj-1","reference":"","scope":"live"})");
  }
}

TEST_CASE("CardReferenceRoutes maps core resolution outcomes", "[card-reference-routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", (dir / "project").string());

  constexpr const char* live_id = "12345678-1111-4111-8111-111111111111";
  constexpr const char* second_live_id = "12345678-2222-4222-8222-222222222222";
  constexpr const char* trashed_id = "77777777-7777-4777-8777-777777777777";
  holder::card::CardRepo cards(db);
  create_card(cards, live_id, "Live card");
  create_card(cards, second_live_id, "Another live card");
  create_card(cards, trashed_id, "Trashed card", 42);

  SECTION("resolved full ID") {
    const auto res =
        resolve(db, {{"project_id", "proj-1"}, {"reference", live_id}, {"scope", "live"}});
    REQUIRE(res.result() == http::status::ok);
    const auto data = nlohmann::json::parse(res.body()).at("data");
    REQUIRE(data["status"] == "resolved");
    REQUIRE(data["match_kind"] == "full_id");
    REQUIRE(data["card"]["card_id"] == live_id);
    REQUIRE(data["card"]["title"] == "Live card");
    REQUIRE(data["card"]["deleted_at"].is_null());
  }

  SECTION("ambiguous ID prefix") {
    const auto res =
        resolve(db, {{"project_id", "proj-1"}, {"reference", "12345678"}, {"scope", "either"}});
    REQUIRE(res.result() == http::status::ok);
    const auto data = nlohmann::json::parse(res.body()).at("data");
    REQUIRE(data["status"] == "ambiguous");
    REQUIRE(data["match_kind"] == "id_prefix");
    REQUIRE(data["candidates"].size() == 2);
    REQUIRE(data["candidates"][0]["card_id"] == live_id);
    REQUIRE(data["candidates"][1]["card_id"] == second_live_id);
  }

  SECTION("resolved exact title in trash") {
    const auto res = resolve(
        db,
        {{"project_id", "proj-1"}, {"reference", "Trashed card"}, {"scope", "trashed"}}
    );
    REQUIRE(res.result() == http::status::ok);
    const auto data = nlohmann::json::parse(res.body()).at("data");
    REQUIRE(data["status"] == "resolved");
    REQUIRE(data["match_kind"] == "exact_title");
    REQUIRE(data["card"]["card_id"] == trashed_id);
    REQUIRE(data["card"]["deleted_at"] == 42);
  }

  SECTION("not found in requested scope") {
    const auto res =
        resolve(db, {{"project_id", "proj-1"}, {"reference", "Trashed card"}, {"scope", "live"}});
    REQUIRE(res.result() == http::status::ok);
    const auto data = nlohmann::json::parse(res.body()).at("data");
    REQUIRE(data == nlohmann::json{{"status", "not_found"}});
  }
}

TEST_CASE(
    "CardReferenceRoutes preserves project and server error semantics",
    "[card-reference-routes]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  const auto body = nlohmann::json{
      {"project_id", "missing"},
      {"reference", "12345678"},
      {"scope", "live"},
  };

  SECTION("missing project") {
    const auto res = resolve(db, body);
    REQUIRE(res.result() == http::status::not_found);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["code"] == "not_found");
    REQUIRE(payload["error"]["message"] == "Project not found.");
  }

  SECTION("database failure") {
    db.close();
    const auto res = resolve(db, body);
    REQUIRE(res.result() == http::status::internal_server_error);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["code"] == "error");
  }
}

TEST_CASE(
    "AuthenticatedRoutes dispatches the card reference endpoint",
    "[card-reference-routes][authenticated-routes]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");
  holder::test::create_project(db, "proj-1", (dir / "project").string());

  auto req = make_request(
      http::verb::post,
      "/card-references/resolve",
      R"({"project_id":"proj-1","reference":"Missing","scope":"live"})"
  );
  http::response<http::string_body> res;
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  const auto uuid_v4 = []() {
    return std::string("unused");
  };

  const auto result = holder::api::routes::dispatch_authenticated_routes(
      "/card-references/resolve",
      "",
      req,
      res,
      socket,
      db,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      nullptr,
      uuid_v4
  );
  REQUIRE_FALSE(result.streamed);
  REQUIRE(res.result() == http::status::ok);
  REQUIRE(nlohmann::json::parse(res.body())["data"]["status"] == "not_found");
}

TEST_CASE("OpenAPI documents card reference resolution", "[openapi][card-reference-routes]") {
  std::ifstream input(OPENAPI_YAML_PATH);
  REQUIRE(input.is_open());
  const std::string document(
      (std::istreambuf_iterator<char>(input)),
      std::istreambuf_iterator<char>()
  );

  REQUIRE(document.find("  /card-references/resolve:\n") != std::string::npos);
  REQUIRE(document.find("    CardReferenceResolveRequest:\n") != std::string::npos);
  REQUIRE(document.find("required: [project_id, reference, scope]") != std::string::npos);
  REQUIRE(document.find("enum: [live, trashed, either]") != std::string::npos);
  REQUIRE(document.find("enum: [resolved, ambiguous, not_found]") != std::string::npos);
  REQUIRE(document.find("enum: [full_id, id_prefix, exact_title]") != std::string::npos);
}
