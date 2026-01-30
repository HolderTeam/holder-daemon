#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;

TEST_CASE("HTTP ai capabilities returns not configured when runtime missing", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db;
  db.open(db_path);

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

  const auto caps = http_json_request(bound.bind,
                                      bound.port,
                                      token,
                                      boost::beast::http::verb::get,
                                      "/ai/capabilities",
                                      nlohmann::json{},
                                      boost::beast::http::status::ok);
  REQUIRE(caps["ok"] == true);
  REQUIRE(caps["data"]["runner_available"] == false);
  REQUIRE(caps["data"]["error"].is_string());

  const auto retry = http_json_request(bound.bind,
                                       bound.port,
                                       token,
                                       boost::beast::http::verb::post,
                                       "/ai/runner/retry",
                                       nlohmann::json{},
                                       boost::beast::http::status::not_implemented);
  REQUIRE(retry["ok"] == false);
  REQUIRE(retry["error"]["code"] == "not_implemented");

  std::raise(SIGTERM);
  server_thread.join();
}
