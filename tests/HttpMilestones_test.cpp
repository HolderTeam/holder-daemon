#include "http_test_helpers.h"

#include "model/Card.h"

using holder::test::create_project;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

namespace {

struct RunningServer {
  holder::index::FtsIndexer fts;
  holder::card::CardStore cards;
  holder::api::HttpServer server;
  holder::core::SignalHandler signals;
  holder::api::HttpServer::BoundInfo bound;
  std::unique_ptr<holder::test::HttpServerThreadGuard> thread;

  RunningServer(holder::platform::Db& db, const std::string& token)
      : fts(db),
        cards(db, &fts),
        server("127.0.0.1", 0, db, token, &cards, &fts) {
    try {
      bound = server.start();
    } catch (const std::exception& ex) {
      SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
    }
    thread = std::make_unique<holder::test::HttpServerThreadGuard>(server, signals);
    REQUIRE(holder::test::wait_for_http_listener(bound.bind, bound.port));
  }
};

holder::model::Card card(
    const std::string& card_id,
    const std::string& title,
    long long created_at,
    long long updated_at
) {
  holder::model::Card value;
  value.card_id = card_id;
  value.project_id = "proj-1";
  value.title = title;
  value.created_at = created_at;
  value.updated_at = updated_at;
  return value;
}

} // namespace

TEST_CASE(
    "HTTP milestones persist through cards and feed the project calendar",
    "[http][milestones]"
) {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);

  running.cards.create(card("card-a", "Car records", 100, 100), "Car notes\n");
  running.cards.create(card("card-b", "House records", 150, 250), "House notes\n");

  const auto created = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/card-a/milestones",
      {{"start_at", 200},
       {"end_at", 220},
       {"all_day", false},
       {"kind", "Service"},
       {"description", "Annual service"}},
      boost::beast::http::status::created
  );
  REQUIRE(created["data"]["card_id"] == "card-a");
  REQUIRE(created["data"]["start_at"] == 200);
  REQUIRE(created["data"]["end_at"] == 220);
  REQUIRE(created["data"]["kind"] == "Service");
  const auto milestone_id = created["data"]["milestone_id"].get<std::string>();
  const auto milestone_created_at = created["data"]["created_at"].get<long long>();

  const auto updated = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/cards/card-a/milestones/" + milestone_id,
      {{"kind", "Renewal"}},
      boost::beast::http::status::ok
  );
  REQUIRE(updated["data"]["milestone_id"] == milestone_id);
  REQUIRE(updated["data"]["card_id"] == "card-a");
  REQUIRE(updated["data"]["start_at"] == 200);
  REQUIRE(updated["data"]["end_at"] == 220);
  REQUIRE(updated["data"]["all_day"] == false);
  REQUIRE(updated["data"]["kind"] == "Renewal");
  REQUIRE(updated["data"]["description"] == "Annual service");
  REQUIRE(updated["data"]["created_at"] == milestone_created_at);
  REQUIRE(updated["data"]["updated_at"].get<long long>() >= milestone_created_at);

  const auto listed = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/milestones",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["milestone_id"] == milestone_id);
  REQUIRE(listed["data"][0]["kind"] == "Renewal");

  const auto calendar = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/calendar?project_id=proj-1&from=50&to=300",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(calendar["data"]["milestones"].size() == 1);
  REQUIRE(calendar["data"]["milestones"][0]["card_title"] == "Car records");
  REQUIRE(calendar["data"]["created_cards"].size() == 2);
  REQUIRE(calendar["data"]["updated_cards"].size() == 1);
  REQUIRE(calendar["data"]["updated_cards"][0]["card_id"] == "card-b");

  const auto removed = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/cards/card-a/milestones/" + milestone_id,
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(removed["data"]["removed"] == true);

  const auto after_delete = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/card-a/milestones",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(after_delete["data"].empty());
}

TEST_CASE("HTTP milestone and calendar routes validate inputs", "[http][milestones]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);
  running.cards.create(card("card-a", "Card", 100, 100), "Notes\n");

  const auto missing_range = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/calendar?project_id=proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::bad_request
  );
  REQUIRE(missing_range["error"]["code"] == "bad_request");

  const auto reversed_range = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/calendar?project_id=proj-1&from=20&to=10",
      nlohmann::json::object(),
      boost::beast::http::status::bad_request
  );
  REQUIRE(reversed_range["error"]["code"] == "bad_request");

  const auto bad_end = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/card-a/milestones",
      {{"start_at", 200}, {"end_at", 100}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(bad_end["error"]["code"] == "bad_request");

  const auto created = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/card-a/milestones",
      {{"start_at", 200}, {"end_at", 220}, {"kind", "Review"}},
      boost::beast::http::status::created
  );
  const auto milestone_id = created["data"]["milestone_id"].get<std::string>();

  const auto invalid_update = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/cards/card-a/milestones/" + milestone_id,
      {{"start_at", 221}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(invalid_update["error"]["code"] == "bad_request");

  const auto empty_update = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/cards/card-a/milestones/" + milestone_id,
      nlohmann::json::object(),
      boost::beast::http::status::bad_request
  );
  REQUIRE(empty_update["error"]["code"] == "bad_request");

  const auto unknown_field = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/cards/card-a/milestones/" + milestone_id,
      {{"title", "Not a milestone field"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(unknown_field["error"]["code"] == "bad_request");

  running.cards.create(card("card-b", "Other card", 100, 100), "Other\n");
  const auto other = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/cards/card-b/milestones",
      {{"start_at", 300}},
      boost::beast::http::status::created
  );
  const auto other_milestone_id = other["data"]["milestone_id"].get<std::string>();
  const auto wrong_card = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/cards/card-a/milestones/" + other_milestone_id,
      {{"kind", "Wrong card"}},
      boost::beast::http::status::not_found
  );
  REQUIRE(wrong_card["error"]["code"] == "not_found");

  const auto missing_milestone = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/cards/card-a/milestones/00000000-0000-4000-8000-000000000000",
      {{"kind", "Missing"}},
      boost::beast::http::status::not_found
  );
  REQUIRE(missing_milestone["error"]["code"] == "not_found");

  const auto missing_card = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/cards/missing/milestones",
      nlohmann::json::object(),
      boost::beast::http::status::not_found
  );
  REQUIRE(missing_card["error"]["code"] == "not_found");
}

#include "api/routes/MilestoneRoutes.h"

TEST_CASE("Milestone routes validate paths and unavailable card services", "[milestones][routes]") {
  namespace http = boost::beast::http;
  const auto root = make_temp_dir();
  auto db = open_db_with_schema(root / "holder.db");
  create_project(db, "proj-1", (root / "project").string());
  holder::card::CardStore cards(db, nullptr);
  cards.create(card("card-one", "Card", 1, 1), "Card\n");
  auto call = [&](http::verb method,
                  const std::string& path,
                  holder::card::CardStore* store,
                  const std::string& body,
                  const std::map<std::string, std::string>& params = {}) {
    http::request<http::string_body> request{method, path, 11};
    request.body() = body;
    http::response<http::string_body> response;
    const bool handled = holder::api::routes::handle_milestone_routes(
        path,
        request,
        response,
        db,
        store,
        [] {
          return "milestone";
        },
        [&](const std::string& key) {
          auto it = params.find(key);
          return it == params.end() ? "" : it->second;
        }
    );
    return std::pair{handled, response.result()};
  };
  for (const std::string path :
       {"/cards",
        "/cards/id",
        "/cards/id/other",
        "/cards/id/milestones-invalid",
        "/cards/id/milestones/",
        "/cards/id/milestones/a/b"})
    CHECK_FALSE(call(http::verb::get, path, &cards, "{}").first);
  CHECK(
      call(http::verb::get, "/cards//milestones", &cards, "{}").second == http::status::bad_request
  );
  CHECK(call(http::verb::get, "/calendar", &cards, "").second == http::status::bad_request);
  for (auto method : {http::verb::post, http::verb::patch, http::verb::delete_}) {
    const auto path = method == http::verb::post ? "/cards/card-one/milestones"
                                                 : "/cards/card-one/milestones/id";
    CHECK(call(method, path, nullptr, "{}").second == http::status::not_implemented);
  }
  CHECK_FALSE(call(http::verb::put, "/cards/card-one/milestones", &cards, "{}").first);
  for (const std::string body :
       {"{}",
        R"({"start_at":"bad"})",
        R"({"start_at":1,"end_at":"bad"})",
        R"({"start_at":1,"all_day":12})"})
    CHECK(
        call(http::verb::post, "/cards/card-one/milestones", &cards, body).second ==
        http::status::bad_request
    );
  for (const std::string body :
       {"not-json",
        "[]",
        "{}",
        R"({"unknown":true})",
        R"({"start_at":"bad"})",
        R"({"end_at":"bad"})",
        R"({"all_day":12})",
        R"({"kind":12})"})
    CHECK(
        call(http::verb::patch, "/cards/card-one/milestones/id", &cards, body).second ==
        http::status::bad_request
    );
  CHECK(
      call(http::verb::patch, "/cards/card-one/milestones/id", &cards, R"({"kind":"new"})")
          .second == http::status::not_found
  );
  for (const std::map<std::string, std::string> params :
       {std::map<std::string, std::string>{{"project_id", "proj-1"}, {"from", "10"}},
        {{"project_id", "proj-1"}, {"from", "10"}, {"to", "bad"}},
        {{"project_id", "proj-1"}, {"from", "10"}, {"to", "11trailing"}}})
    CHECK(
        call(http::verb::get, "/calendar", &cards, "", params).second == http::status::bad_request
    );
  CHECK(
      call(
          http::verb::get,
          "/calendar",
          &cards,
          "",
          {{"project_id", "missing"}, {"from", "0"}, {"to", "1"}}
      ).second == http::status::not_found
  );
  db.exec("DROP TABLE milestones");
  CHECK(
      call(
          http::verb::get,
          "/calendar",
          &cards,
          "",
          {{"project_id", "proj-1"}, {"from", "0"}, {"to", "1"}}
      ).second == http::status::bad_request
  );
}
