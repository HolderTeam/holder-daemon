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

} // namespace

TEST_CASE("HTTP resources persist complete Git-backed metadata", "[http][resources]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  const auto project_root = dir / "project_repo";
  create_project(db, "proj-1", project_root.string());

  const std::string token = "testtoken";
  RunningServer running(db, token);
  const auto created = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/resources",
      {
          {"project_id", "proj-1"},
          {"type", "website"},
          {"label", "Example"},
          {"metadata",
           {{"identifier", nlohmann::json::array({"https://example.com"})},
            {"creator", nlohmann::json::array({"Ada", "Grace"})}}},
      },
      boost::beast::http::status::created
  );
  const auto resource_id = created["data"]["resource_id"].get<std::string>();
  REQUIRE(created["data"]["assets"].empty());

  const auto listed = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/resources?project_id=proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(listed["data"].size() == 1);
  REQUIRE(listed["data"][0]["type"] == "website");
  REQUIRE(listed["data"][0]["metadata"]["creator"].size() == 2);

  const auto fetched = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/resources/" + resource_id,
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(fetched["data"]["label"] == "Example");

  const auto patched = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/resources/" + resource_id,
      {{"label", "Updated"},
       {"metadata", {{"description", nlohmann::json::array({"Reference"})}}}},
      boost::beast::http::status::ok
  );
  REQUIRE(patched["data"]["label"] == "Updated");
  REQUIRE(patched["data"]["metadata"]["description"][0] == "Reference");
  REQUIRE(patched["data"]["metadata"]["creator"] == nlohmann::json::array({"Ada", "Grace"}));

  const auto removed_creator = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::patch,
      "/resources/" + resource_id,
      {{"metadata", {{"creator", nlohmann::json::array()}}}},
      boost::beast::http::status::ok
  );
  REQUIRE(!removed_creator["data"]["metadata"].contains("creator"));
  REQUIRE(removed_creator["data"]["metadata"]["description"][0] == "Reference");

  REQUIRE(std::filesystem::exists(project_root / "resources"));
  const auto deleted = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/resources/" + resource_id,
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(deleted["ok"] == true);
}

TEST_CASE("HTTP resource routes validate the replacement contract", "[http][resources]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);

  const auto missing_fields = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/resources",
      {{"project_id", "proj-1"}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(missing_fields["error"]["code"] == "bad_request");

  const auto missing_project = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/resources",
      nlohmann::json::object(),
      boost::beast::http::status::bad_request
  );
  REQUIRE(missing_project["error"]["code"] == "bad_request");

  const auto missing_resource = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/resources/no-such-resource",
      nlohmann::json::object(),
      boost::beast::http::status::not_found
  );
  REQUIRE(missing_resource["error"]["code"] == "not_found");
}

TEST_CASE("HTTP local Location import runs as a polled background job", "[http][resources][imports]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());

  holder::index::FtsIndexer setup_fts(db);
  holder::card::CardStore setup_cards(db, &setup_fts);
  holder::model::Card card;
  card.card_id = "card-1234";
  card.project_id = "proj-1";
  card.title = "Homework";
  card.created_at = 10;
  card.updated_at = 10;
  setup_cards.create(card, "Drop the source here.\n");

  const auto source = dir / "homework.pdf";
  std::ofstream(source, std::ios::binary) << "%PDF-1.7\nHolder test\n";
  const auto object_root = dir / "objects";
  const std::string token = "testtoken";
  RunningServer running(db, token);

  const auto location = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations",
      {{"project_id", "proj-1"},
       {"name", "Family Assets"},
       {"provider", "local_directory"},
       {"configuration", nlohmann::json::object()}},
      boost::beast::http::status::created
  );
  REQUIRE(location["data"]["bound"] == false);
  const auto location_id = location["data"]["location_id"].get<std::string>();

  const auto bound = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::put,
      "/locations/" + location_id + "/binding",
      {{"values", {{"root_path", object_root.string()}}}, {"preview", object_root.string()}},
      boost::beast::http::status::ok
  );
  REQUIRE(bound["data"]["bound"] == true);
  REQUIRE(bound["data"].dump().find("root_path") == std::string::npos);

  const auto import_and_wait = [&]() {
    const auto started = http_json_request(
        running.bound.bind,
        running.bound.port,
        token,
        boost::beast::http::verb::post,
        "/imports",
        {{"project_id", "proj-1"},
         {"card_id", "card-1234"},
         {"location_id", location_id},
         {"source_path", source.string()}},
        boost::beast::http::status::accepted
    );
    const auto job_id = started["data"]["job_id"].get<std::string>();

    nlohmann::json job;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
      job = http_json_request(
          running.bound.bind,
          running.bound.port,
          token,
          boost::beast::http::verb::get,
          "/imports/" + job_id,
          nlohmann::json::object(),
          boost::beast::http::status::ok
      )["data"];
      if (job["status"] == "completed" || job["status"] == "failed") break;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } while (std::chrono::steady_clock::now() < deadline);
    return job;
  };

  const auto job = import_and_wait();

  REQUIRE(job["status"] == "completed");
  REQUIRE(job["resource_id"].is_string());
  REQUIRE(job["duplicate_reused"] == false);
  REQUIRE(job["link_created"] == true);
  REQUIRE(std::filesystem::exists(object_root));

  const auto duplicate_job = import_and_wait();
  REQUIRE(duplicate_job["status"] == "completed");
  REQUIRE(duplicate_job["resource_id"] == job["resource_id"]);
  REQUIRE(duplicate_job["asset_id"] == job["asset_id"]);
  REQUIRE(duplicate_job["duplicate_reused"] == true);
  REQUIRE(duplicate_job["link_created"] == false);

  const auto resources = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::get,
      "/resources?project_id=proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(resources["data"].size() == 1);
  REQUIRE(resources["data"][0]["assets"][0]["media_type"] == "application/pdf");
  REQUIRE(resources["data"][0]["referenced_by_cards"].size() == 1);
  REQUIRE(resources["data"][0]["referenced_by_cards"][0]["card_id"] == "card-1234");
  REQUIRE(resources["data"][0]["referenced_by_cards"][0]["title"] == "Homework");
  REQUIRE(
      resources["data"][0]["referenced_by_cards"][0]["link_kinds"] ==
      nlohmann::json::array({"attachment"})
  );
}
