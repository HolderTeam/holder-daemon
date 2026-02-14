#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai router config get/put", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

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

  const auto initial = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::get,
                                         "/ai/router/config",
                                         nlohmann::json{},
                                         boost::beast::http::status::ok);
  REQUIRE(initial["ok"] == true);
  REQUIRE(initial["data"]["effective"]["scope"] == "auto");

  const auto set_global = http_json_request(bound.bind,
                                            bound.port,
                                            token,
                                            boost::beast::http::verb::put,
                                            "/ai/router/config",
                                            nlohmann::json{{"scope", "global"},
                                                           {"router_model", "qwen2.5:0.5b"}},
                                            boost::beast::http::status::ok);
  REQUIRE(set_global["ok"] == true);
  REQUIRE(set_global["data"]["effective"]["scope"] == "global");
  REQUIRE(set_global["data"]["effective"]["router_model"] == "qwen2.5:0.5b");

  const auto set_project = http_json_request(bound.bind,
                                             bound.port,
                                             token,
                                             boost::beast::http::verb::put,
                                             "/ai/router/config",
                                             nlohmann::json{{"scope", "project"},
                                                            {"project_id", "proj-1"},
                                                            {"router_model", "deepseek-r1:latest"}},
                                             boost::beast::http::status::ok);
  REQUIRE(set_project["ok"] == true);
  REQUIRE(set_project["data"]["effective"]["scope"] == "project");
  REQUIRE(set_project["data"]["effective"]["router_model"] == "deepseek-r1:latest");

  const auto get_project = http_json_request(bound.bind,
                                             bound.port,
                                             token,
                                             boost::beast::http::verb::get,
                                             "/ai/router/config?project_id=proj-1",
                                             nlohmann::json{},
                                             boost::beast::http::status::ok);
  REQUIRE(get_project["ok"] == true);
  REQUIRE(get_project["data"]["project"]["router_model"] == "deepseek-r1:latest");

  const auto clear_project = http_json_request(bound.bind,
                                               bound.port,
                                               token,
                                               boost::beast::http::verb::put,
                                               "/ai/router/config",
                                               nlohmann::json{{"scope", "project"},
                                                              {"project_id", "proj-1"},
                                                              {"router_model", nullptr}},
                                               boost::beast::http::status::ok);
  REQUIRE(clear_project["ok"] == true);
  REQUIRE(clear_project["data"]["effective"]["scope"] == "global");

  std::raise(SIGTERM);
  server_thread.join();
}
