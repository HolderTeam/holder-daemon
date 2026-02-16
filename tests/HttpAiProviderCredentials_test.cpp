#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai provider credentials put/get/delete", "[http]") {
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

  const auto put = http_json_request(bound.bind,
                                     bound.port,
                                     token,
                                     boost::beast::http::verb::put,
                                     "/ai/providers/credentials",
                                     nlohmann::json{{"provider", "ChocolateFactory"},
                                                    {"api_key", "cf_test_key_12345"}},
                                     boost::beast::http::status::ok);
  REQUIRE(put["ok"] == true);
  REQUIRE(put["data"]["provider"] == "chocolatefactory");
  REQUIRE(put["data"]["configured"] == true);
  REQUIRE(put["data"]["api_key_preview"].is_string());

  const auto list = http_json_request(bound.bind,
                                      bound.port,
                                      token,
                                      boost::beast::http::verb::get,
                                      "/ai/providers/credentials",
                                      nlohmann::json{},
                                      boost::beast::http::status::ok);
  REQUIRE(list["ok"] == true);
  REQUIRE(list["data"]["providers"].is_array());
  REQUIRE(list["data"]["providers"].size() == 1);
  REQUIRE(list["data"]["providers"][0]["provider"] == "chocolatefactory");

  const auto status = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::get,
                                        "/ai/status",
                                        nlohmann::json{},
                                        boost::beast::http::status::ok);
  REQUIRE(status["ok"] == true);
  REQUIRE(status["data"]["cloud"].is_array());
  REQUIRE(status["data"]["cloud_configured_providers"] == 1);

  const auto settings_put = http_json_request(bound.bind,
                                              bound.port,
                                              token,
                                              boost::beast::http::verb::put,
                                              "/ai/providers/settings",
                                              nlohmann::json{{"provider", "chocolatefactory"},
                                                             {"enabled", false}},
                                              boost::beast::http::status::ok);
  REQUIRE(settings_put["ok"] == true);
  REQUIRE(settings_put["data"]["provider"] == "chocolatefactory");
  REQUIRE(settings_put["data"]["enabled"] == false);

  const auto settings_list = http_json_request(bound.bind,
                                               bound.port,
                                               token,
                                               boost::beast::http::verb::get,
                                               "/ai/providers/settings",
                                               nlohmann::json{},
                                               boost::beast::http::status::ok);
  REQUIRE(settings_list["ok"] == true);
  REQUIRE(settings_list["data"]["providers"].is_array());
  REQUIRE(settings_list["data"]["providers"].size() == 1);
  REQUIRE(settings_list["data"]["providers"][0]["provider"] == "chocolatefactory");
  REQUIRE(settings_list["data"]["providers"][0]["enabled"] == false);

  const auto removed = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::delete_,
                                         "/ai/providers/credentials/chocolatefactory",
                                         nlohmann::json{},
                                         boost::beast::http::status::ok);
  REQUIRE(removed["ok"] == true);

  const auto settings_after_remove = http_json_request(bound.bind,
                                                       bound.port,
                                                       token,
                                                       boost::beast::http::verb::get,
                                                       "/ai/providers/settings",
                                                       nlohmann::json{},
                                                       boost::beast::http::status::ok);
  REQUIRE(settings_after_remove["ok"] == true);
  REQUIRE(settings_after_remove["data"]["providers"].is_array());
  REQUIRE(settings_after_remove["data"]["providers"].size() == 1);
  REQUIRE(settings_after_remove["data"]["providers"][0]["provider"] == "chocolatefactory");
  REQUIRE(settings_after_remove["data"]["providers"][0]["enabled"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}
