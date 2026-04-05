#include "http_test_helpers.h"

#include "api/Listener.h"
#include "api/Router.h"
#include "ai/NudgeService.h"
#include "index/FtsIndexer.h"
#include "card/CardStore.h"
#include "llm/RunnerRegistry.h"
#include "platform/Signal.h"

#include <atomic>
#include <future>

using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::create_project;

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

TEST_CASE("Listener worker-owned DB handles support concurrent mixed request load", "[listener]") {
  namespace http = boost::beast::http;

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1", (dir / "project").string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);

  holder::api::Router router;
  const std::string token = "testtoken";
  holder::api::Listener listener("127.0.0.1",
                                 0,
                                 db,
                                 token,
                                 router,
                                 std::chrono::steady_clock::now(),
                                 &card_store,
                                 &fts,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 &runner_registry);
  holder::api::Listener::BoundInfo bound;
  try {
    bound = listener.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread listener_thread([&listener, &signals]() { listener.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  for (int i = 1; i <= 3; ++i) {
    const auto card_id = "card-" + std::to_string(i);
    const auto created = holder::test::http_json_request(
        bound.bind,
        bound.port,
        token,
        http::verb::post,
        "/cards",
        nlohmann::json{
            {"card_id", card_id},
            {"project_id", "proj-1"},
            {"title", "Card " + std::to_string(i)},
            {"content", "start"},
            {"created_at", i},
            {"updated_at", i},
        },
        http::status::created);
    REQUIRE(created["ok"] == true);
  }

  auto list_future = std::async(std::launch::async, [&]() {
    return holder::test::http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           http::verb::get,
                                           "/cards?project_id=proj-1",
                                           nlohmann::json::object(),
                                           http::status::ok);
  });
  auto project_future = std::async(std::launch::async, [&]() {
    return holder::test::http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           http::verb::get,
                                           "/projects",
                                           nlohmann::json::object(),
                                           http::status::ok);
  });
  auto status_future = std::async(std::launch::async, [&]() {
    return holder::test::http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           http::verb::get,
                                           "/ai/status",
                                           nlohmann::json::object(),
                                           http::status::ok);
  });
  auto runner_future = std::async(std::launch::async, [&]() {
    return holder::test::http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           http::verb::post,
                                           "/ai/runners",
                                           nlohmann::json{
                                               {"name", "Concurrent Runner"},
                                               {"kind", "ollama"},
                                               {"base_url", "http://concurrent:11434"},
                                           },
                                           http::status::created);
  });

  auto card_future = std::async(std::launch::async, [&]() {
    return holder::test::http_json_request(bound.bind,
                                           bound.port,
                                           token,
                                           http::verb::get,
                                           "/cards/card-1",
                                           nlohmann::json::object(),
                                           http::status::ok);
  });

  const auto listed = list_future.get();
  const auto projects = project_future.get();
  const auto status = status_future.get();
  const auto runner = runner_future.get();
  const auto card = card_future.get();

  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"].is_array());
  REQUIRE(projects["ok"] == true);
  REQUIRE(projects["data"].is_array());
  REQUIRE(status["ok"] == true);
  REQUIRE(status["data"]["runners"].is_array());
  REQUIRE(runner["ok"] == true);
  REQUIRE(runner["data"]["runner_id"].get<std::string>().rfind("manual-", 0) == 0);
  REQUIRE(card["ok"] == true);
  REQUIRE(card["data"]["card_id"] == "card-1");

  listener.stop();
  listener_thread.join();
}

TEST_CASE("Listener serves card nudge and ai status routes without regression", "[listener]") {
  namespace http = boost::beast::http;

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1", (dir / "project").string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);
  holder::ai::NudgeService nudge_service(db, &runner_registry);

  holder::api::Router router;
  const std::string token = "testtoken";
  holder::api::Listener listener("127.0.0.1",
                                 0,
                                 db,
                                 token,
                                 router,
                                 std::chrono::steady_clock::now(),
                                 &card_store,
                                 &fts,
                                 &nudge_service,
                                 nullptr,
                                 nullptr,
                                 &runner_registry);
  holder::api::Listener::BoundInfo bound;
  try {
    bound = listener.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread listener_thread([&listener, &signals]() { listener.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto created = holder::test::http_json_request(bound.bind,
                                                       bound.port,
                                                       token,
                                                       http::verb::post,
                                                       "/cards",
                                                       nlohmann::json{
                                                           {"card_id", "card-1"},
                                                           {"project_id", "proj-1"},
                                                           {"title", "Frog"},
                                                           {"content", ""},
                                                           {"created_at", 1},
                                                           {"updated_at", 1},
                                                       },
                                                       http::status::created);
  REQUIRE(created["ok"] == true);

  const auto status = holder::test::http_json_request(bound.bind,
                                                      bound.port,
                                                      token,
                                                      http::verb::get,
                                                      "/ai/status",
                                                      nlohmann::json::object(),
                                                      http::status::ok);
  REQUIRE(status["ok"] == true);
  REQUIRE(status["data"]["runners"].is_array());

  const auto listed = holder::test::http_json_request(bound.bind,
                                                      bound.port,
                                                      token,
                                                      http::verb::get,
                                                      "/ai/nudges?project_id=proj-1&card_id=card-1",
                                                      nlohmann::json::object(),
                                                      http::status::ok);
  REQUIRE(listed["ok"] == true);
  REQUIRE(listed["data"]["nudges"].is_array());

  const auto patched = holder::test::http_json_request(bound.bind,
                                                       bound.port,
                                                       token,
                                                       http::verb::patch,
                                                       "/cards/card-1",
                                                       nlohmann::json{
                                                           {"title", "Frog Updated"},
                                                           {"content", "Now has body"},
                                                           {"updated_at", 2},
                                                       },
                                                       http::status::ok);
  REQUIRE(patched["ok"] == true);

  listener.stop();
  listener_thread.join();
}

TEST_CASE("Multiple configured runners do not block card save path under background saturation",
          "[listener]") {
  namespace http = boost::beast::http;

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);
  create_project(db, "proj-1", (dir / "project").string());
  holder::index::FtsIndexer fts(db);
  holder::card::CardStore card_store(db, &fts);
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);

  holder::api::Router router;
  std::atomic<int> slow_started{0};
  router.add(http::verb::get, "/ai/slow",
             [&slow_started](const holder::api::Router::Request&, holder::api::Router::Response& res) {
               slow_started.fetch_add(1);
               std::this_thread::sleep_for(std::chrono::milliseconds(300));
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
                                 &card_store,
                                 &fts,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 &runner_registry);
  holder::api::Listener::BoundInfo bound;
  try {
    bound = listener.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread listener_thread([&listener, &signals]() { listener.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto created = holder::test::http_json_request(bound.bind,
                                                       bound.port,
                                                       token,
                                                       http::verb::post,
                                                       "/cards",
                                                       nlohmann::json{
                                                           {"card_id", "card-1"},
                                                           {"project_id", "proj-1"},
                                                           {"title", "Card"},
                                                           {"content", "start"},
                                                           {"created_at", 1},
                                                           {"updated_at", 1},
                                                       },
                                                       http::status::created);
  REQUIRE(created["ok"] == true);

  for (int i = 0; i < 3; ++i) {
    const auto created_runner = holder::test::http_json_request(
        bound.bind,
        bound.port,
        token,
        http::verb::post,
        "/ai/runners",
        nlohmann::json{
            {"name", "Runner " + std::to_string(i + 1)},
            {"kind", "ollama"},
            {"base_url", "http://runner" + std::to_string(i + 1) + ":11434"},
        },
        http::status::created);
    REQUIRE(created_runner["ok"] == true);
  }

  auto slow1 = std::async(std::launch::async, [&]() {
    return holder::test::http_request_raw(bound.bind, bound.port, token, http::verb::get, "/ai/slow");
  });
  auto slow2 = std::async(std::launch::async, [&]() {
    return holder::test::http_request_raw(bound.bind, bound.port, token, http::verb::get, "/ai/slow");
  });
  auto slow3 = std::async(std::launch::async, [&]() {
    return holder::test::http_request_raw(bound.bind, bound.port, token, http::verb::get, "/ai/slow");
  });

  for (int i = 0; i < 50 && slow_started.load() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  REQUIRE(slow_started.load() == 3);

  const auto save_started = std::chrono::steady_clock::now();
  const auto patched = holder::test::http_json_request(bound.bind,
                                                       bound.port,
                                                       token,
                                                       http::verb::patch,
                                                       "/cards/card-1",
                                                       nlohmann::json{
                                                           {"title", "Saved While Busy"},
                                                           {"content", "updated"},
                                                           {"updated_at", 2},
                                                       },
                                                       http::status::ok);
  const auto save_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - save_started)
                                   .count();
  REQUIRE(patched["ok"] == true);
  REQUIRE(save_elapsed_ms < 200);

  REQUIRE(slow1.get().status == http::status::ok);
  REQUIRE(slow2.get().status == http::status::ok);
  REQUIRE(slow3.get().status == http::status::ok);

  listener.stop();
  listener_thread.join();
}
