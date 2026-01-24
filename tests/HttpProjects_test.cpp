#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::ensure_uuid_seeded;
using holder::test::EnvGuard;

TEST_CASE("HTTP project create/list/get/patch", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  ensure_uuid_seeded();

  const auto projects_root = dir / "projects_root";
  std::filesystem::create_directories(projects_root);
  EnvGuard root_env("HOLDER_PROJECTS_ROOT", projects_root.string());

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });

  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  nlohmann::json create_body = {
      {"project_id", "proj-1"},
      {"name", "Project One"},
      {"root_path", "/tmp/project"},
      {"created_at", 10},
      {"updated_at", 10}
  };

  const auto created = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::post,
                                         "/projects",
                                         create_body,
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);

  nlohmann::json create_auto = {
      {"name", "Auto Project"}
  };

  const auto created_auto = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::post,
                                              "/projects",
                                              create_auto,
                                              boost::beast::http::status::created);
  REQUIRE(created_auto["ok"] == true);
  REQUIRE(created_auto["data"]["project_id"].is_string());
  REQUIRE(created_auto["data"]["project_id"].get<std::string>().size() > 0);
  const std::string auto_id = created_auto["data"]["project_id"].get<std::string>();

  const auto fetched_auto = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/projects/" + auto_id,
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(fetched_auto["data"]["created_at"].get<long long>() > 0);
  REQUIRE(fetched_auto["data"]["updated_at"].get<long long>() > 0);
  REQUIRE(fetched_auto["data"]["root_path"].is_string());
  REQUIRE(fetched_auto["data"]["root_path"].get<std::string>().size() > 0);
  const std::string auto_root = fetched_auto["data"]["root_path"].get<std::string>();
  REQUIRE(auto_root.rfind(projects_root.string(), 0) == 0);

  const auto listed = http_json_request(bound.bind, bound.port, token,
                                        boost::beast::http::verb::get,
                                        "/projects",
                                        nlohmann::json::object(),
                                        boost::beast::http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(listed["data"].size() >= 2);

  const auto fetched = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/projects/proj-1",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(fetched["ok"] == true);
  REQUIRE(fetched["data"]["name"] == "Project One");

  nlohmann::json update_body = {
      {"name", "Project Uno"},
      {"updated_at", 20}
  };

  const auto updated = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::patch,
                                         "/projects/proj-1",
                                         update_body,
                                         boost::beast::http::status::ok);
  REQUIRE(updated["ok"] == true);

  const auto fetched_after = http_json_request(bound.bind, bound.port, token,
                                               boost::beast::http::verb::get,
                                               "/projects/proj-1",
                                               nlohmann::json::object(),
                                               boost::beast::http::status::ok);
  REQUIRE(fetched_after["data"]["name"] == "Project Uno");

  nlohmann::json create_body2 = {
      {"project_id", "proj-2"},
      {"name", "Second Project"},
      {"root_path", "/tmp/project2"},
      {"created_at", 15},
      {"updated_at", 30}
  };

  http_json_request(bound.bind, bound.port, token,
                    boost::beast::http::verb::post,
                    "/projects",
                    create_body2,
                    boost::beast::http::status::created);

  const auto filtered_by_name = http_json_request(bound.bind, bound.port, token,
                                                  boost::beast::http::verb::get,
                                                  "/projects?name=Second",
                                                  nlohmann::json::object(),
                                                  boost::beast::http::status::ok);
  REQUIRE(filtered_by_name["data"].size() == 1);
  REQUIRE(filtered_by_name["data"][0]["project_id"] == "proj-2");

  const auto filtered_by_updated = http_json_request(bound.bind, bound.port, token,
                                                     boost::beast::http::verb::get,
                                                     "/projects?updated_after=21&updated_before=40",
                                                     nlohmann::json::object(),
                                                     boost::beast::http::status::ok);
  REQUIRE(filtered_by_updated["data"].size() == 1);
  REQUIRE(filtered_by_updated["data"][0]["project_id"] == "proj-2");

  const auto filtered_ordered = http_json_request(bound.bind, bound.port, token,
                                                  boost::beast::http::verb::get,
                                                  "/projects?order=updated_at_asc&limit=1&offset=0",
                                                  nlohmann::json::object(),
                                                  boost::beast::http::status::ok);
  REQUIRE(filtered_ordered["data"].size() == 1);
  REQUIRE(filtered_ordered["data"][0]["project_id"] == "proj-1");

  const auto name_ordered = http_json_request(bound.bind, bound.port, token,
                                              boost::beast::http::verb::get,
                                              "/projects?order=name_desc&limit=1&offset=0",
                                              nlohmann::json::object(),
                                              boost::beast::http::status::ok);
  REQUIRE(name_ordered["data"].size() == 1);
  REQUIRE(name_ordered["data"][0]["project_id"] == "proj-2");

  const auto created_ordered = http_json_request(bound.bind, bound.port, token,
                                                 boost::beast::http::verb::get,
                                                 "/projects?order=created_at_asc&limit=1&offset=0",
                                                 nlohmann::json::object(),
                                                 boost::beast::http::status::ok);
  REQUIRE(created_ordered["data"].size() == 1);
  REQUIRE(created_ordered["data"][0]["project_id"] == "proj-1");

  const auto deleted = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::delete_,
                                         "/projects/proj-1",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(deleted["ok"] == true);

  const auto missing = http_json_request(bound.bind, bound.port, token,
                                         boost::beast::http::verb::get,
                                         "/projects/proj-1",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::not_found);
  REQUIRE(missing["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}
