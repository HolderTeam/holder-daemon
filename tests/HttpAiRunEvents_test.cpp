#include "http_test_helpers.h"
#include "ai/AiRunRepo.h"

using holder::test::http_request_raw;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai run events streams terminal status", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::store::AiRunRepo repo(db);
  holder::model::AiRun run;
  run.run_id = "run-1";
  run.project_id = "proj-1";
  run.thread_id = "thread-1";
  run.mode = "auto";
  run.prompt = "hello";
  run.status = "completed";
  run.chosen_model = "fake-echo";
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
  std::thread server_thread([&server, &signals]() { server.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto res = http_request_raw(bound.bind,
                                    bound.port,
                                    token,
                                    boost::beast::http::verb::get,
                                    "/ai/runs/run-1/events");
  REQUIRE(res.status == boost::beast::http::status::ok);
  REQUIRE(res.content_type.find("text/event-stream") != std::string::npos);
  REQUIRE(res.body.find("event: done") != std::string::npos);
  REQUIRE(res.body.find("\"run_id\":\"run-1\"") != std::string::npos);

  std::raise(SIGTERM);
  server_thread.join();
}
