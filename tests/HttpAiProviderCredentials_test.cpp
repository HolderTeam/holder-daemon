#include "http_test_helpers.h"
#include "api/routes/ai/providers/AiProviderCredentialRoutes.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_req(http::verb method,
                                          const std::string& target,
                                          const std::string& body = "") {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body;
  }
  return req;
}
} // namespace

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

TEST_CASE("AiProviderCredentialRoutes direct validation and error branches", "[http]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  http::response<http::string_body> res;

  SECTION("settings put missing provider/enabled") {
    auto req = make_req(http::verb::put, "/ai/providers/settings", nlohmann::json::object().dump());
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/settings", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Missing provider or enabled.");
  }

  SECTION("settings put invalid provider") {
    auto req = make_req(http::verb::put,
                        "/ai/providers/settings",
                        nlohmann::json{{"provider", "!!!"}, {"enabled", true}}.dump());
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/settings", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("settings put malformed json catches exception") {
    auto req = make_req(http::verb::put, "/ai/providers/settings", "{");
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/settings", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("settings delete invalid provider") {
    auto req = make_req(http::verb::delete_, "/ai/providers/settings/%");
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/settings/%", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Invalid provider.");
  }

  SECTION("settings delete success") {
    auto put = make_req(http::verb::put,
                        "/ai/providers/settings",
                        nlohmann::json{{"provider", "switchyard"}, {"enabled", true}}.dump());
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/settings", put, res, db));
    REQUIRE(res.result() == http::status::ok);

    auto del = make_req(http::verb::delete_, "/ai/providers/settings/switchyard");
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/settings/switchyard", del, res, db));
    REQUIRE(res.result() == http::status::ok);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["data"]["provider"] == "switchyard");
  }

  SECTION("credentials put missing provider/api_key") {
    auto req = make_req(http::verb::put, "/ai/providers/credentials", nlohmann::json::object().dump());
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/credentials", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Missing provider or api_key.");
  }

  SECTION("credentials put invalid provider") {
    auto req = make_req(http::verb::put,
                        "/ai/providers/credentials",
                        nlohmann::json{{"provider", "!!!"}, {"api_key", "x"}}.dump());
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/credentials", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("credentials put empty api_key after trim") {
    auto req = make_req(http::verb::put,
                        "/ai/providers/credentials",
                        nlohmann::json{{"provider", "switchyard"}, {"api_key", "   "}}.dump());
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/credentials", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "api_key cannot be empty.");
  }

  SECTION("credentials put malformed json catches exception") {
    auto req = make_req(http::verb::put, "/ai/providers/credentials", "{");
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/credentials", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
  }

  SECTION("credentials delete invalid provider") {
    auto req = make_req(http::verb::delete_, "/ai/providers/credentials/%");
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/credentials/%", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "Invalid provider.");
  }

  SECTION("unmatched route returns false") {
    auto req = make_req(http::verb::get, "/ai/providers/credentials");
    REQUIRE_FALSE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        "/ai/providers/unknown", req, res, db));
  }
}

TEST_CASE("AiProviderCredentialRoutes direct DB exception branches", "[http]") {
  holder::platform::Db unopened;
  http::response<http::string_body> res;

  auto expect_bad_request = [&](http::verb method, const std::string& path, const std::string& body = "") {
    auto req = make_req(method, path, body);
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_credential_routes(
        path, req, res, unopened));
    REQUIRE(res.result() == http::status::bad_request);
  };

  expect_bad_request(http::verb::get, "/ai/providers/settings");
  expect_bad_request(http::verb::put,
                     "/ai/providers/settings",
                     nlohmann::json{{"provider", "switchyard"}, {"enabled", true}}.dump());
  expect_bad_request(http::verb::delete_, "/ai/providers/settings/switchyard");

  expect_bad_request(http::verb::get, "/ai/providers/credentials");
  expect_bad_request(http::verb::put,
                     "/ai/providers/credentials",
                     nlohmann::json{{"provider", "switchyard"}, {"api_key", "key"}}.dump());
  expect_bad_request(http::verb::delete_, "/ai/providers/credentials/switchyard");
}
