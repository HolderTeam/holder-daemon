#include "http_test_helpers.h"

#include "api/Listener.h"
#include "api/Router.h"
#include "platform/Signal.h"

#include <atomic>
#include <future>

using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("Listener start fails when port already in use", "[listener]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server1("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server1.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::api::HttpServer server2("127.0.0.1", bound.port, db, token, nullptr, nullptr);
  REQUIRE_THROWS(server2.start());
}

TEST_CASE("HttpServer run throws if start was not called", "[listener]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::core::SignalHandler signals;
  REQUIRE_THROWS(server.run(signals));
}

TEST_CASE("Slow background route does not block foreground route", "[listener]") {
  namespace http = boost::beast::http;

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  holder::api::Router router;
  std::atomic<bool> slow_started{false};
  router.add(http::verb::get, "/ai/slow",
             [&slow_started](const holder::api::Router::Request&, holder::api::Router::Response& res) {
               slow_started.store(true);
               std::this_thread::sleep_for(std::chrono::milliseconds(250));
               res.result(http::status::ok);
               res.set(http::field::content_type, "application/json");
               res.body() = R"({"ok":true})";
               res.prepare_payload();
             });
  router.add(http::verb::get, "/foreground-fast",
             [](const holder::api::Router::Request&, holder::api::Router::Response& res) {
               res.result(http::status::ok);
               res.set(http::field::content_type, "application/json");
               res.body() = R"({"ok":true})";
               res.prepare_payload();
             });

  const std::string token = "testtoken";
  holder::api::Listener listener("127.0.0.1",
                                 0,
                                 db,
                                 token,
                                 router,
                                 std::chrono::steady_clock::now(),
                                 nullptr,
                                 nullptr,
                                 nullptr);
  holder::api::Listener::BoundInfo bound;
  try {
    bound = listener.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread listener_thread([&listener, &signals]() { listener.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto slow_future = std::async(std::launch::async, [&]() {
    return holder::test::http_request_raw(bound.bind, bound.port, token, http::verb::get, "/ai/slow");
  });

  for (int i = 0; i < 50 && !slow_started.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(slow_started.load());

  const auto fast_started = std::chrono::steady_clock::now();
  const auto fast = holder::test::http_request_raw(
      bound.bind, bound.port, token, http::verb::get, "/foreground-fast");
  const auto fast_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - fast_started)
                                   .count();

  REQUIRE(fast.status == http::status::ok);
  REQUIRE(fast_elapsed_ms < 200);

  const auto slow = slow_future.get();
  REQUIRE(slow.status == http::status::ok);

  listener.stop();
  listener_thread.join();
}

TEST_CASE("Slow background route does not block save lane route", "[listener]") {
  namespace http = boost::beast::http;

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  holder::api::Router router;
  std::atomic<bool> slow_started{false};
  router.add(http::verb::get, "/ai/slow",
             [&slow_started](const holder::api::Router::Request&, holder::api::Router::Response& res) {
               slow_started.store(true);
               std::this_thread::sleep_for(std::chrono::milliseconds(250));
               res.result(http::status::ok);
               res.set(http::field::content_type, "application/json");
               res.body() = R"({"ok":true})";
               res.prepare_payload();
             });
  router.add(http::verb::patch, "/cards/save-test",
             [](const holder::api::Router::Request&, holder::api::Router::Response& res) {
               res.result(http::status::ok);
               res.set(http::field::content_type, "application/json");
               res.body() = R"({"ok":true})";
               res.prepare_payload();
             });

  const std::string token = "testtoken";
  holder::api::Listener listener("127.0.0.1",
                                 0,
                                 db,
                                 token,
                                 router,
                                 std::chrono::steady_clock::now(),
                                 nullptr,
                                 nullptr,
                                 nullptr);
  holder::api::Listener::BoundInfo bound;
  try {
    bound = listener.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread listener_thread([&listener, &signals]() { listener.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto slow_future = std::async(std::launch::async, [&]() {
    return holder::test::http_request_raw(bound.bind, bound.port, token, http::verb::get, "/ai/slow");
  });

  for (int i = 0; i < 50 && !slow_started.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(slow_started.load());

  const auto save_started = std::chrono::steady_clock::now();
  const auto saved = holder::test::http_json_request(bound.bind,
                                                     bound.port,
                                                     token,
                                                     http::verb::patch,
                                                     "/cards/save-test",
                                                     nlohmann::json::object(),
                                                     http::status::ok);
  const auto save_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - save_started)
                                   .count();

  REQUIRE(saved["ok"] == true);
  REQUIRE(save_elapsed_ms < 200);

  const auto slow = slow_future.get();
  REQUIRE(slow.status == http::status::ok);

  listener.stop();
  listener_thread.join();
}
