#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai provider catalog reflects configured credentials", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto cloudproviders_path = std::filesystem::path(SCHEMA_SQL_PATH).parent_path().parent_path() /
                                   "config" / "cloudproviders.yaml";
  holder::test::EnvGuard cloudproviders_env("HOLDER_CLOUDPROVIDERS_PATH", cloudproviders_path.string());

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

  const auto before = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::get,
                                        "/ai/providers/catalog",
                                        nlohmann::json{},
                                        boost::beast::http::status::ok);
  REQUIRE(before["ok"] == true);
  REQUIRE(before["data"]["providers"].is_array());

  bool found_chocolatefactory = false;
  for (const auto& provider : before["data"]["providers"]) {
    if (provider["id"] == "chocolatefactory") {
      found_chocolatefactory = true;
      REQUIRE(provider["configured"] == false);
    }
  }
  REQUIRE(found_chocolatefactory);

  const auto put = http_json_request(bound.bind,
                                     bound.port,
                                     token,
                                     boost::beast::http::verb::put,
                                     "/ai/providers/credentials",
                                     nlohmann::json{{"provider", "chocolatefactory"},
                                                    {"api_key", "cf_test_key_abc"}},
                                     boost::beast::http::status::ok);
  REQUIRE(put["ok"] == true);

  const auto after = http_json_request(bound.bind,
                                       bound.port,
                                       token,
                                       boost::beast::http::verb::get,
                                       "/ai/providers/catalog",
                                       nlohmann::json{},
                                       boost::beast::http::status::ok);
  REQUIRE(after["ok"] == true);
  bool configured_chocolatefactory = false;
  for (const auto& provider : after["data"]["providers"]) {
    if (provider["id"] == "chocolatefactory") {
      configured_chocolatefactory = provider["configured"].get<bool>();
      REQUIRE(provider["auth"].is_object());
      REQUIRE_FALSE(provider["auth"].contains("credential_provider_key"));
    }
  }
  REQUIRE(configured_chocolatefactory);

  std::raise(SIGTERM);
  server_thread.join();
}
