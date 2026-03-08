#include "http_test_helpers.h"
#include "api/support/CloudClient.h"
#include "ai/AiProviderCredentialRepo.h"
#include "ai/AiRunRepo.h"
#include "ai/AiMessageRepo.h"

using holder::test::make_temp_dir;

namespace {

class CloudRunOverrideGuard {
 public:
  explicit CloudRunOverrideGuard(holder::api::support::CloudModelRunnerOverride fn) {
    holder::api::support::set_run_cloud_model_override_for_tests(std::move(fn));
  }
  ~CloudRunOverrideGuard() {
    holder::api::support::clear_run_cloud_model_override_for_tests();
  }
};

} // namespace

TEST_CASE("HTTP ai runs post stores run and messages", "[http]") {
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

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
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

TEST_CASE("HTTP ai runs provider request forces cloud even when local runner is ready", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig& provider,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string* error) -> std::optional<std::string> {
        if (provider.id != "switchyard" || model.id != "openrouter/auto") {
          if (error) *error = "unexpected provider/model";
          return std::nullopt;
        }
        return std::string("forced cloud output");
      });

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog.yaml";
  {
    std::ofstream out(cloud_cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: openrouter/auto\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::store::Db db = holder::test::open_db_with_schema(db_path);
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  holder::store::AiProviderCredentialRepo cred_repo(db);
  cred_repo.upsert("switchyard", "test-key", 1, 1);

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
      {"prompt", "force cloud path please"},
      {"project_id", "proj-1"},
      {"provider", "switchyard"},
  };

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
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
  socket.shutdown(tcp::socket::shutdown_both);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  holder::store::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_project("proj-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "completed");
  REQUIRE(runs[0].chosen_model == std::optional<std::string>("switchyard:openrouter/auto"));
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto policy_trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(policy_trace["path"] == "cloud");
  REQUIRE(policy_trace["provider"] == "switchyard");

  std::raise(SIGTERM);
  server_thread.join();
}
