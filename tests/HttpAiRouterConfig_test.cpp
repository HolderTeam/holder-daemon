#include "http_test_helpers.h"
#include "api/routes/ai/status/AiRouterConfigRoutes.h"

#include <boost/beast/http.hpp>

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

TEST_CASE("HTTP ai router config validates input and handles error branches", "[http]") {
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

  auto missing_scope = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::put,
                                         "/ai/router/config",
                                         nlohmann::json{{"router_model", "x"}},
                                         boost::beast::http::status::bad_request);
  REQUIRE(missing_scope["error"]["code"] == "bad_request");

  auto bad_scope = http_json_request(bound.bind,
                                     bound.port,
                                     token,
                                     boost::beast::http::verb::put,
                                     "/ai/router/config",
                                     nlohmann::json{{"scope", "team"}},
                                     boost::beast::http::status::bad_request);
  REQUIRE(bad_scope["error"]["code"] == "bad_request");

  auto missing_project = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::put,
                                           "/ai/router/config",
                                           nlohmann::json{{"scope", "project"}, {"router_model", "m"}},
                                           boost::beast::http::status::bad_request);
  REQUIRE(missing_project["error"]["code"] == "bad_request");

  auto null_project = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::put,
                                        "/ai/router/config",
                                        nlohmann::json{{"scope", "project"}, {"project_id", nullptr}},
                                        boost::beast::http::status::bad_request);
  REQUIRE(null_project["error"]["code"] == "bad_request");

  auto empty_project = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::put,
                                         "/ai/router/config",
                                         nlohmann::json{{"scope", "project"}, {"project_id", ""}},
                                         boost::beast::http::status::bad_request);
  REQUIRE(empty_project["error"]["code"] == "bad_request");

  auto unknown_project = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::put,
                                           "/ai/router/config",
                                           nlohmann::json{{"scope", "project"}, {"project_id", "nope"}},
                                           boost::beast::http::status::not_found);
  REQUIRE(unknown_project["error"]["code"] == "not_found");

  auto set_global = http_json_request(bound.bind,
                                      bound.port,
                                      token,
                                      boost::beast::http::verb::put,
                                      "/ai/router/config",
                                      nlohmann::json{{"scope", "global"}, {"router_model", "qwen"}},
                                      boost::beast::http::status::ok);
  REQUIRE(set_global["data"]["effective"]["scope"] == "global");

  auto clear_global = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::put,
                                        "/ai/router/config",
                                        nlohmann::json{{"scope", "global"}, {"router_model", nullptr}},
                                        boost::beast::http::status::ok);
  REQUIRE(clear_global["data"]["effective"]["scope"] == "auto");

  auto raw_bad_json = holder::test::http_request_raw(bound.bind,
                                                     bound.port,
                                                     token,
                                                     boost::beast::http::verb::put,
                                                     "/ai/router/config");
  REQUIRE(raw_bad_json.status == boost::beast::http::status::bad_request);

  db.exec("DROP TABLE ai_router_config;");
  auto get_fail = http_json_request(bound.bind,
                                    bound.port,
                                    token,
                                    boost::beast::http::verb::get,
                                    "/ai/router/config",
                                    nlohmann::json{},
                                    boost::beast::http::status::bad_request);
  REQUIRE(get_fail["error"]["code"] == "bad_request");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("AiRouterConfigRoutes returns false for unrelated path", "[http]") {
  namespace http = boost::beast::http;
  auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");

  http::request<http::string_body> req{http::verb::get, "/not-router", 11};
  http::response<http::string_body> res;
  auto param_get = [](const std::string&) { return std::string(); };

  const bool handled = holder::api::routes::ai::status::handle_ai_router_config_routes(
      "/not-router", req, res, db, param_get);
  REQUIRE(handled == false);
}
