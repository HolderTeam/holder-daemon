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

TEST_CASE("HTTP milestones persist through cards and feed the project calendar", "[http][milestones]") {
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
