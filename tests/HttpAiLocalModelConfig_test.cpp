#include "http_test_helpers.h"
#include "api/routes/ai/status/AiLocalModelConfigRoutes.h"

#include <boost/beast/http.hpp>

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai local model config get/put", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);

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
                                         "/ai/local-models/config",
                                         nlohmann::json{},
                                         boost::beast::http::status::ok);
  REQUIRE(initial["ok"] == true);
  REQUIRE(initial["data"]["fast_model"].is_null());
  REQUIRE(initial["data"]["strong_model"].is_null());
  REQUIRE(initial["data"]["deep_model"].is_null());

  const auto set_all = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::put,
                                         "/ai/local-models/config",
                                         nlohmann::json{{"fast_model", "qwen-fast"},
                                                        {"strong_model", "qwen-strong"},
                                                        {"deep_model", "qwen-deep"}},
                                         boost::beast::http::status::ok);
  REQUIRE(set_all["ok"] == true);
  REQUIRE(set_all["data"]["fast_model"] == "auto-local::qwen-fast");
  REQUIRE(set_all["data"]["strong_model"] == "auto-local::qwen-strong");
  REQUIRE(set_all["data"]["deep_model"] == "auto-local::qwen-deep");

  const auto clear_all = http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           boost::beast::http::verb::put,
                                           "/ai/local-models/config",
                                           nlohmann::json{{"fast_model", nullptr},
                                                          {"strong_model", nullptr},
                                                          {"deep_model", nullptr}},
                                           boost::beast::http::status::ok);
  REQUIRE(clear_all["ok"] == true);
  REQUIRE(clear_all["data"]["fast_model"].is_null());
  REQUIRE(clear_all["data"]["strong_model"].is_null());
  REQUIRE(clear_all["data"]["deep_model"].is_null());

  const auto empty_string_clears = http_json_request(bound.bind,
                                                     bound.port,
                                                     token,
                                                     boost::beast::http::verb::put,
                                                     "/ai/local-models/config",
                                                     nlohmann::json{{"fast_model", ""},
                                                                    {"strong_model", "qwen-strong"}},
                                                     boost::beast::http::status::ok);
  REQUIRE(empty_string_clears["ok"] == true);
  REQUIRE(empty_string_clears["data"]["fast_model"].is_null());
  REQUIRE(empty_string_clears["data"]["strong_model"] == "auto-local::qwen-strong");
  REQUIRE(empty_string_clears["data"]["deep_model"].is_null());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai local model config validates input and handles error branches", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);

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

  auto raw_bad_json = holder::test::http_request_raw(bound.bind,
                                                     bound.port,
                                                     token,
                                                     boost::beast::http::verb::put,
                                                     "/ai/local-models/config");
  REQUIRE(raw_bad_json.status == boost::beast::http::status::bad_request);

  db.exec("DROP TABLE ai_local_model_config;");
  auto get_fail = http_json_request(bound.bind,
                                    bound.port,
                                    token,
                                    boost::beast::http::verb::get,
                                    "/ai/local-models/config",
                                    nlohmann::json{},
                                    boost::beast::http::status::bad_request);
  REQUIRE(get_fail["error"]["code"] == "bad_request");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("AiLocalModelConfigRoutes returns false for unrelated path", "[http]") {
  namespace http = boost::beast::http;
  auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");

  http::request<http::string_body> req{http::verb::get, "/not-local-model-config", 11};
  http::response<http::string_body> res;

  const bool handled = holder::api::routes::ai::status::handle_ai_local_model_config_routes(
      "/not-local-model-config", req, res, db);
  REQUIRE(handled == false);
}
