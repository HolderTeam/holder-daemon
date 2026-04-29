#include "http_test_helpers.h"
#include "ai/AiMessageRepo.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP ai messages capture creates thread and two messages", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto project_root = dir / "project";

  auto db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '" + project_root.string() + "', 1, 1);");

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

  const auto created = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::post,
                                         "/ai/messages/capture",
                                         nlohmann::json{{"project_id", "proj-1"},
                                                        {"prompt", "What is this?"},
                                                        {"response", "A captured response."},
                                                        {"source", "manual_paste"},
                                                        {"provider", "Gemini"},
                                                        {"model", "gemma-3-12b"},
                                                        {"url", "https://example.com"}},
                                         boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  REQUIRE(created["data"]["thread_id"].is_string());
  REQUIRE(created["data"]["user_message_id"].is_string());
  REQUIRE(created["data"]["assistant_message_id"].is_string());

  const std::string thread_id = created["data"]["thread_id"].get<std::string>();
  holder::ai::AiMessageRepo msg_repo(db, nullptr);
  const auto msgs = msg_repo.list_by_thread(thread_id);
  REQUIRE(msgs.size() == 2);
  REQUIRE(msgs[0].role == "user");
  REQUIRE(msgs[1].role == "assistant");
  REQUIRE(msgs[1].provider.has_value());
  REQUIRE(msgs[1].provider.value() == "Gemini");

  std::raise(SIGTERM);
  server_thread.join();
}
