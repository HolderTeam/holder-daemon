#include "ai/AiRunRepo.h"
#include "http_test_helpers.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;

TEST_CASE("HTTP ai runs list and get", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
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

  const auto list = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs?thread_id=thread-1",
      nlohmann::json{},
      boost::beast::http::status::ok
  );
  REQUIRE(list["ok"] == true);
  REQUIRE(list["data"].is_array());
  REQUIRE(list["data"].size() == 1);
  REQUIRE(list["data"][0]["run_id"] == "run-1");

  const auto fetched = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/run-1",
      nlohmann::json{},
      boost::beast::http::status::ok
  );
  REQUIRE(fetched["ok"] == true);
  REQUIRE(fetched["data"]["run_id"] == "run-1");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs list requires project_id or thread_id", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);

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

  const auto out = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs",
      nlohmann::json{},
      boost::beast::http::status::bad_request
  );
  REQUIRE(out["ok"] == false);
  REQUIRE(out["error"]["code"] == "bad_request");
  REQUIRE(out["error"]["message"] == "Missing project_id or thread_id.");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs list can query by project_id", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-proj";
  run.project_id = "proj-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "completed";
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

  const auto out = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs?project_id=proj-1",
      nlohmann::json{},
      boost::beast::http::status::ok
  );
  REQUIRE(out["ok"] == true);
  REQUIRE(out["data"].is_array());
  REQUIRE(out["data"].size() == 1);
  REQUIRE(out["data"][0]["run_id"] == "run-proj");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE(
    "HTTP ai runs get supports legacy policy trace fallback and malformed JSON handling",
    "[http]"
) {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);

  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  holder::ai::AiRunRepo repo(db);
  holder::model::AiRun legacy;
  legacy.run_id = "run-legacy";
  legacy.project_id = "proj-1";
  legacy.mode = "auto";
  legacy.prompt = "legacy";
  legacy.ranked_json = std::string("{\"path\":\"cloud\",\"attempts\":[]}");
  legacy.status = "completed";
  legacy.created_at = 1;
  legacy.updated_at = 2;
  repo.create(legacy);

  holder::model::AiRun malformed;
  malformed.run_id = "run-malformed";
  malformed.project_id = "proj-1";
  malformed.mode = "auto";
  malformed.prompt = "badjson";
  malformed.policy_trace_json = std::string("{");
  malformed.ranked_json = std::string("[1,2,3]");
  malformed.status = "failed";
  malformed.created_at = 1;
  malformed.updated_at = 2;
  repo.create(malformed);

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

  const auto legacy_out = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/run-legacy",
      nlohmann::json{},
      boost::beast::http::status::ok
  );
  REQUIRE(legacy_out["ok"] == true);
  REQUIRE(legacy_out["data"]["policy_trace"].is_object());
  REQUIRE(legacy_out["data"]["policy_trace"]["path"] == "cloud");

  const auto malformed_out = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/run-malformed",
      nlohmann::json{},
      boost::beast::http::status::ok
  );
  REQUIRE(malformed_out["ok"] == true);
  REQUIRE(malformed_out["data"]["policy_trace"].is_null());

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs get returns not_found for missing id", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);

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

  const auto out = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/ai/runs/no-such-run",
      nlohmann::json{},
      boost::beast::http::status::not_found
  );
  REQUIRE(out["ok"] == false);
  REQUIRE(out["error"]["code"] == "not_found");

  std::raise(SIGTERM);
  server_thread.join();
}
