#include "http_test_helpers.h"
#include "store/AiRunRepo.h"
#include "store/AiMessageRepo.h"

using holder::test::make_temp_dir;

TEST_CASE("HTTP ai complete stores run and messages", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::store::Db db = holder::test::open_db_with_schema(db_path);
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO cards(card_id, project_id, title, rel_path, created_at, updated_at) "
          "VALUES('card-1', 'proj-1', 'Card', 'cards/ca/rd/card-1.md', 1, 1);");

  const std::string token = "testtoken";
  holder::store::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner);

  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));
  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  nlohmann::json body = {
    {"prompt", "hello world"},
    {"project_id", "proj-1"},
    {"context", { {"card_id", "card-1"}, {"card_title", "First"}, {"card_body", "Body"} }}
  };

  http::request<http::string_body> req{http::verb::post, "/ai/complete", 11};
  req.set(http::field::host, bound.bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);
  req.set(http::field::content_type, "application/json");
  req.body() = body.dump();
  req.prepare_payload();

  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);
  if (res.result() != http::status::ok) {
    FAIL(res.body());
  }
  REQUIRE(res.result() == http::status::ok);
  socket.shutdown(tcp::socket::shutdown_both);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  holder::store::AiThreadRepo thread_repo(db);
  const auto threads = thread_repo.list("proj-1");
  REQUIRE(threads.size() == 1);

  holder::store::AiMessageRepo msg_repo(db, nullptr);
  const auto msgs = msg_repo.list_by_thread(threads[0].thread_id);
  REQUIRE(msgs.size() == 2);

  holder::store::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_project("proj-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].message_id.has_value());

  std::raise(SIGTERM);
  server_thread.join();
}
