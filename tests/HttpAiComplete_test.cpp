#include "http_test_helpers.h"
#include "api/routes/ai/runs/AiRunPostRoute.h"
#include "api/support/CloudClient.h"
#include "ai/AiProviderCredentialRepo.h"
#include "ai/AiProviderSettingRepo.h"
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

class ServerThreadGuard {
 public:
  ServerThreadGuard(holder::api::HttpServer& server, holder::core::SignalHandler& signals)
      : thread_([&server, &signals]() { server.run(signals); }) {}

  ~ServerThreadGuard() {
    if (thread_.joinable()) {
      std::raise(SIGTERM);
      thread_.join();
    }
  }

 private:
  std::thread thread_;
};

} // namespace

TEST_CASE("AiRunPostRoute cloud path stores context and compaction trace for thread runs", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig&,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> { return std::string("cloud output"); });

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog.yaml";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

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
    out << "  summary_refresh:\n";
    out << "    trigger_context_tokens: 1\n";
    out << "    source_context_tokens: 256\n";
    out << "    response_tokens_budget: 64\n";
    out << "    max_summary_chars: 2048\n";
    out << "    min_interval_seconds: 0\n";
    out << "    min_delta_tokens: 0\n";
    out << "    force_refresh_tokens: 1\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::ai::AiProviderCredentialRepo cred_repo(db);
  cred_repo.upsert("switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
  const auto endpoint = acceptor.local_endpoint();
  tcp::socket client(ioc);
  client.connect(endpoint);
  tcp::socket server_socket(ioc);
  acceptor.accept(server_socket);

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "openrouter/auto"},
                              {"context", {{"card_id", "card-1"}, {"card_body", "x"}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].context_json.has_value());
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["reason"] == "no_compact_model");

  holder::ai::AiMessageRepo msg_repo(db, nullptr);
  const auto msgs = msg_repo.list_by_thread("thread-1");
  bool saw_user_model = false;
  for (const auto& msg : msgs) {
    if (msg.role == "user" && msg.model.has_value() && msg.model.value() == "openrouter/auto") {
      saw_user_model = true;
    }
  }
  REQUIRE(saw_user_model);

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud path returns early when SSE header write fails", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog.yaml";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

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
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  holder::ai::AiProviderCredentialRepo cred_repo(db);
  cred_repo.upsert("switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{
      {"prompt", "cloud prompt"},
      {"project_id", "proj-1"},
      {"provider", "switchyard"},
      {"model", "openrouter/auto"},
  }
                   .dump();
  req.prepare_payload();

  boost::asio::io_context ioc;
  tcp::socket unopened_socket(ioc);
  http::response<http::string_body> res;
  int id_seq = 1;
  auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      unopened_socket,
      db,
      nullptr,
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_project("proj-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "started");
}

TEST_CASE("HTTP ai runs post stores run and messages", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO cards(card_id, project_id, title, rel_path, created_at, updated_at) "
          "VALUES('card-1', 'proj-1', 'Card', 'cards/ca/rd/card-1.md', 1, 1);");

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
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

  holder::ai::AiThreadRepo thread_repo(db);
  const auto threads = thread_repo.list("proj-1");
  REQUIRE(threads.size() == 1);

  holder::ai::AiMessageRepo msg_repo(db, nullptr);
  const auto msgs = msg_repo.list_by_thread(threads[0].thread_id);
  REQUIRE(msgs.size() == 2);

  holder::ai::AiRunRepo run_repo(db);
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

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  holder::ai::AiProviderCredentialRepo cred_repo(db);
  cred_repo.upsert("switchyard", "test-key", 1, 1);

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
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

  holder::ai::AiRunRepo run_repo(db);
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

TEST_CASE("HTTP ai runs rejects bad input payloads", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
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

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, bound.bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);
  req.set(http::field::content_type, "application/json");
  req.body() = "{";
  req.prepare_payload();
  http::write(socket, req);

  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);
  REQUIRE(res.result() == http::status::bad_request);
  socket.shutdown(tcp::socket::shutdown_both);

  const auto missing_prompt = holder::test::http_json_request(bound.bind,
                                                              bound.port,
                                                              token,
                                                              http::verb::post,
                                                              "/ai/runs",
                                                              nlohmann::json{{"mode", "auto"}},
                                                              http::status::bad_request);
  REQUIRE(missing_prompt["ok"] == false);
  REQUIRE(missing_prompt["error"]["code"] == "bad_request");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs auto-creates thread with 80-char title cap", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  const std::string long_prompt(140, 'x');
  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
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
      {"prompt", long_prompt},
      {"project_id", "proj-1"},
      {"mode", "model"},
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
  REQUIRE(res.result() == http::status::ok);
  socket.shutdown(tcp::socket::shutdown_both);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  holder::ai::AiThreadRepo thread_repo(db);
  const auto threads = thread_repo.list("proj-1");
  REQUIRE(threads.size() == 1);
  REQUIRE(threads[0].title.size() == 80);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs cloud path rejects disabled requested provider", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

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
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  holder::ai::AiProviderCredentialRepo cred_repo(db);
  cred_repo.upsert("switchyard", "test-key", 1, 1);
  holder::ai::AiProviderSettingRepo setting_repo(db);
  setting_repo.upsert("switchyard", false, 2);

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
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

  const auto out = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/ai/runs",
      nlohmann::json{{"prompt", "hello"}, {"project_id", "proj-1"}, {"provider", "switchyard"}},
      boost::beast::http::status::service_unavailable);
  REQUIRE(out["ok"] == false);
  REQUIRE(out["error"]["code"] == "cloud_not_configured");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs cloud path rejects unknown requested model", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

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
    out << "        role: default\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  holder::ai::AiProviderCredentialRepo cred_repo(db);
  cred_repo.upsert("switchyard", "test-key", 1, 1);

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
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

  const auto out = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/ai/runs",
      nlohmann::json{{"prompt", "hello"},
                     {"project_id", "proj-1"},
                     {"provider", "switchyard"},
                     {"model", "not-installed"}},
      boost::beast::http::status::service_unavailable);
  REQUIRE(out["ok"] == false);
  REQUIRE(out["error"]["code"] == "cloud_not_configured");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs local path accepts explicit thread_id and forced installed model", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  (void)runner.retry();
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  ServerThreadGuard server_thread(server, signals);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));
  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, bound.bind);
  req.set(http::field::user_agent, "holder-tests");
  req.set(http::field::authorization, "Bearer " + token);
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "hello thread"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"model", "fake-echo"}}
                   .dump();
  req.prepare_payload();

  http::write(socket, req);
  boost::beast::flat_buffer buffer;
  http::response<http::string_body> res;
  http::read(socket, buffer, res);
  socket.shutdown(tcp::socket::shutdown_both);

  if (res.result() != http::status::ok) {
    FAIL(res.body());
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].mode == "model");
  REQUIRE(runs[0].thread_id == std::optional<std::string>("thread-1"));
  REQUIRE(runs[0].status == "completed");

  holder::ai::AiMessageRepo msg_repo(db, nullptr);
  const auto msgs = msg_repo.list_by_thread("thread-1");
  REQUIRE(msgs.size() == 2);
  bool saw_user = false;
  bool saw_model = false;
  for (const auto& m : msgs) {
    if (m.role == "user") {
      saw_user = true;
    }
    if (m.model.has_value() && m.model.value() == "fake-echo") {
      saw_model = true;
    }
  }
  REQUIRE(saw_user);
  REQUIRE(saw_model);
}

TEST_CASE("HTTP ai runs local path rejects forced model that is not installed", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
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

  const auto out = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/ai/runs",
      nlohmann::json{{"prompt", "hello"}, {"project_id", "proj-1"}, {"model", "missing-model"}},
      boost::beast::http::status::bad_request);
  REQUIRE(out["ok"] == false);
  REQUIRE(out["error"]["code"] == "bad_request");
  REQUIRE(out["error"]["message"] == "Requested model is not installed.");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs cloud path returns not configured when no enabled provider has creds", "[http]") {
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
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

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

  const auto out = holder::test::http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/ai/runs",
      nlohmann::json{{"prompt", "hello"}, {"project_id", "proj-1"}},
      boost::beast::http::status::service_unavailable);
  REQUIRE(out["ok"] == false);
  REQUIRE(out["error"]["code"] == "cloud_not_configured");

  std::raise(SIGTERM);
  server_thread.join();
}
