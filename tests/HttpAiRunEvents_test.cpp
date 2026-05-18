#include "ai/AiRunRepo.h"
#include "api/routes/ai/runs/AiRunQueryRoutes.h"
#include "api/support/RunEventStore.h"
#include "http_test_helpers.h"

namespace http = boost::beast::http;

using holder::test::http_request_raw;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai run events streams terminal status", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-1";
  run.project_id = "proj-1";
  run.thread_id = "thread-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "completed";
  run.chosen_model = "auto-local::fake-echo";
  run.created_at = 1;
  run.updated_at = 2;
  repo.create(run);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto res = http_request_raw(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/run-1/events"
  );
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("text/event-stream") != std::string::npos);
  REQUIRE(res.body.find("event: done") != std::string::npos);
  REQUIRE(res.body.find("\"run_id\":\"run-1\"") != std::string::npos);
  REQUIRE(res.body.find("\"runner_id\":\"auto-local\"") != std::string::npos);
  REQUIRE(res.body.find("\"model_ref\":\"auto-local::fake-echo\"") != std::string::npos);
  REQUIRE(res.body.find("\"model\":\"fake-echo\"") != std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai run events streams terminal failed status with error", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-failed-1";
  run.project_id = "proj-1";
  run.thread_id = "thread-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "failed";
  run.error = "boom";
  run.created_at = 1;
  run.updated_at = 2;
  repo.create(run);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto res = http_request_raw(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/run-failed-1/events"
  );
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("text/event-stream") != std::string::npos);
  REQUIRE(res.body.find("event: failed") != std::string::npos);
  REQUIRE(res.body.find("\"error\":\"boom\"") != std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE(
    "HTTP ai run events terminal status falls back to raw chosen_model when not a model ref",
    "[http]"
) {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-plain-model-1";
  run.project_id = "proj-1";
  run.thread_id = "thread-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "completed";
  run.chosen_model = "plain-model";
  run.created_at = 1;
  run.updated_at = 2;
  repo.create(run);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto res = http_request_raw(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/run-plain-model-1/events"
  );
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("text/event-stream") != std::string::npos);
  REQUIRE(res.body.find("event: done") != std::string::npos);
  REQUIRE(res.body.find("\"model_ref\":\"plain-model\"") != std::string::npos);
  REQUIRE(res.body.find("\"model\":\"plain-model\"") != std::string::npos);
  REQUIRE(res.body.find("\"runner_id\"") == std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai run events returns not_found for missing run", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db = open_db_with_schema(db_path);
  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto res = http_request_raw(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/does-not-exist/events"
  );
  REQUIRE(res.status == boost::beast::http::status::not_found);
  REQUIRE(res.body.find("Run not found.") != std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("Ai run events route handles direct empty/missing identifiers", "[http]") {
  boost::asio::io_context ioc;
  boost::asio::ip::tcp::socket socket(ioc);
  http::response<http::string_body> res;

  SECTION("empty run_id path returns not_found") {
    holder::platform::Db db = open_db_with_schema(make_temp_dir() / "holder.db");
    auto out = holder::api::routes::ai::runs::handle_ai_runs_events_route(
        "/ai/runs//events",
        socket,
        res,
        db
    );
    REQUIRE(out.handled == true);
    REQUIRE(out.streamed == false);
    REQUIRE(res.result() == http::status::not_found);
  }

  SECTION("db exception during get returns not_found") {
    holder::platform::Db unopened_db;
    auto out = holder::api::routes::ai::runs::handle_ai_runs_events_route(
        "/ai/runs/run-1/events",
        socket,
        res,
        unopened_db
    );
    REQUIRE(out.handled == true);
    REQUIRE(out.streamed == false);
    REQUIRE(res.result() == http::status::not_found);
  }
}

TEST_CASE("HTTP ai run events streams from in-memory event stream", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = open_db_with_schema(db_path);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-stream-1";
  run.project_id = "proj-1";
  run.thread_id = "thread-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "started";
  run.created_at = 1;
  run.updated_at = 2;
  repo.create(run);

  holder::api::support::append_run_event("run-stream-1", "progress", {{"step", 1}}, false);
  holder::api::support::append_run_event("run-stream-1", "done", {{"status", "completed"}}, true);

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto res = http_request_raw(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/run-stream-1/events"
  );
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("text/event-stream") != std::string::npos);
  REQUIRE(res.body.find("event: progress") != std::string::npos);
  REQUIRE(res.body.find("event: done") != std::string::npos);
  REQUIRE(res.body.find("\"run_id\":\"run-stream-1\"") != std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("RunEventStore trims in-memory event stream to last 512 events", "[http]") {
  const std::string run_id = "run-trim-coverage-1";
  for (int i = 0; i < 520; ++i) {
    holder::api::support::append_run_event(run_id, "progress", {{"idx", i}}, i == 519);
  }

  const auto stream_opt = holder::api::support::get_run_event_stream(run_id);
  REQUIRE(stream_opt.has_value());
  REQUIRE(stream_opt->events.size() == 512);
  REQUIRE(stream_opt->events.front().data["idx"] == 8);
  REQUIRE(stream_opt->events.back().data["idx"] == 519);
  REQUIRE(stream_opt->finished == true);
}
