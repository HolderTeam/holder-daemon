#include "http_test_helpers.h"
#include "api/routes/ai/runs/AiRunPostRoute.h"
#include "api/support/CloudClient.h"
#include "api/support/CloudQuota.h"
#include "api/support/ThreadCompaction.h"
#include "api/support/Time.h"
#include "ai/AiLocalModelConfigRepo.h"
#include "ai/AiProviderCredentialRepo.h"
#include "ai/AiProviderSettingRepo.h"
#include "ai/AiRunnerRepo.h"
#include "ai/AiRunRepo.h"
#include "ai/AiThreadRepo.h"
#include "ai/AiMessageRepo.h"
#include "llm/LocalRunnerClient.h"
#include "llm/LocalModelRunner.h"
#include "privacy/SecretStore.h"

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

namespace {

void seed_provider_credential(holder::platform::Db& db,
                              holder::privacy::SecretStore& secret_store,
                              const std::string& provider,
                              const std::string& api_key,
                              long long created_at,
                              long long updated_at) {
  static constexpr const char* kSecretService = "holder.ai_provider_credentials";
  const std::string preview = "stored-preview";
  holder::ai::AiProviderCredentialRepo cred_repo(db);
  secret_store.set(kSecretService, provider, api_key, preview, created_at, updated_at);
  cred_repo.upsert(provider, preview, created_at, updated_at);
}

} // namespace

TEST_CASE("AiRunPostRoute cloud path stores context and compaction trace for thread runs", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig&,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> { return std::string("cloud output"); });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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

  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

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
      secret_store.get(),
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
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{
      {"prompt", "cloud prompt"},
      {"project_id", "proj-1"},
      {"thread_id", "thread-1"},
      {"provider", "switchyard"},
      {"model", "openrouter/auto"},
      {"context", {{"card_id", "card-1"}, {"card_title", "Card"}, {"card_body", "Body"}}},
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
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "started");
  REQUIRE(runs[0].context_json.has_value());
}

TEST_CASE("AiRunPostRoute direct returns runner_unavailable when cloud catalog missing", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto missing_cfg = dir / "missing_ai_catalog.yaml";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  {
    std::ofstream out(missing_cfg);
    REQUIRE(out.is_open());
    out << "invalid: true\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", missing_cfg.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{
      {"prompt", "cloud prompt"},
      {"project_id", "proj-1"},
  }
                   .dump();
  req.prepare_payload();

  boost::asio::io_context ioc;
  tcp::socket unopened_socket(ioc);
  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      unopened_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed == false);
  REQUIRE(res.result() == http::status::service_unavailable);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["error"]["code"] == "runner_unavailable");
}

TEST_CASE("AiRunPostRoute direct catches DB failures from thread creation path", "[http]") {
  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  holder::platform::Db unopened_db;
  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{
      {"prompt", "needs thread"},
      {"project_id", "proj-1"},
  }
                   .dump();
  req.prepare_payload();

  boost::asio::io_context ioc;
  tcp::socket unopened_socket(ioc);
  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      unopened_socket,
      unopened_db,
      nullptr,
      nullptr,
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(res.result() == http::status::bad_request);
}

TEST_CASE("AiRunPostRoute cloud path selects provider via ordered fallback", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_fallback.yaml";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  {
    std::ofstream out(cloud_cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider: First\n";
    out << "        provider_id: first\n";
    out << "        credential_key: first\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: model-a\n";
    out << "        endpoint: /v1/chat\n";
    out << "      - provider: Second\n";
    out << "        provider_id: second\n";
    out << "        credential_key: second\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: model-b\n";
    out << "        endpoint: /v1/chat\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  seed_provider_credential(db, *secret_store, "second", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{
      {"prompt", "fallback provider selection"},
      {"project_id", "proj-1"},
  }
                   .dump();
  req.prepare_payload();

  boost::asio::io_context ioc;
  tcp::socket unopened_socket(ioc);
  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      unopened_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_project("proj-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "started");
}

TEST_CASE("AiRunPostRoute local routing uses router ranking and truncates router context", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  holder::ai::AiLocalModelConfigRepo local_model_cfg_repo(db);
  local_model_cfg_repo.set(std::string("router-model"), std::nullopt, std::nullopt, 1);

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {
      holder::llm::LocalModel{.name = "router-model", .size = 1},
      holder::llm::LocalModel{.name = "model-a", .size = 100},
      holder::llm::LocalModel{.name = "model-b", .size = 200},
  };
  runner.set_status_override_for_tests(status);

  bool saw_router_call = false;
  bool saw_model_a_call = false;
  bool saw_model_b_call = false;
  bool router_prompt_was_truncated = false;
  runner.set_stream_generate_override_for_tests(
      [&](const std::string& model,
          const std::string& prompt,
          const std::string&,
          const std::function<void(const std::string&)>& on_chunk,
          std::string* error) -> bool {
        if (model == "router-model") {
          saw_router_call = true;
          on_chunk("[\"model-a\",\"model-b\"]");
          router_prompt_was_truncated = prompt.size() < 54000;
          return true;
        }
        if (model == "model-a") {
          saw_model_a_call = true;
          on_chunk("partial-a");
          if (error) *error = "model-a failed";
          return false;
        }
        if (model == "model-b") {
          saw_model_b_call = true;
          on_chunk("final-b");
          return true;
        }
        if (error) *error = "unexpected model";
        return false;
      });

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  const std::string huge_context(70000, 'x');
  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "route locally"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"context", {{"card_id", "card-1"}, {"card_body", huge_context}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      nullptr,
      &runner_registry,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  REQUIRE(saw_router_call);
  REQUIRE(saw_model_a_call);
  REQUIRE(saw_model_b_call);
  REQUIRE(router_prompt_was_truncated);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "completed");
  REQUIRE(runs[0].chosen_model == std::optional<std::string>("auto-local::model-b"));
  REQUIRE(runs[0].ranked_json.has_value());
  REQUIRE(runs[0].ranked_json.value().find("model-a") != std::string::npos);

  holder::ai::AiMessageRepo msg_repo(db, nullptr);
  const auto msgs = msg_repo.list_by_thread("thread-1");
  bool saw_assistant = false;
  for (const auto& msg : msgs) {
    if (msg.role == "assistant" && msg.model.has_value() && msg.model.value() == "auto-local::model-b") {
      saw_assistant = true;
    }
  }
  REQUIRE(saw_assistant);

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute local routing falls back to largest model when router ranking is invalid", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {
      holder::llm::LocalModel{.name = "router-model", .size = 1},
      holder::llm::LocalModel{.name = "small-model", .size = 10},
      holder::llm::LocalModel{.name = "large-model", .size = 1000},
  };
  runner.set_status_override_for_tests(status);
  runner.set_stream_generate_override_for_tests(
      [&](const std::string& model,
          const std::string&,
          const std::string&,
          const std::function<void(const std::string&)>& on_chunk,
          std::string*) -> bool {
        if (model == "router-model") {
          on_chunk("not-json");
          return true;
        }
        if (model == "large-model") {
          on_chunk("large-output");
          return true;
        }
        return false;
      });

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "route locally"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      nullptr,
      &runner_registry,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "completed");
  REQUIRE(runs[0].chosen_model == std::optional<std::string>("auto-local::large-model"));
  REQUIRE(runs[0].ranked_json == std::optional<std::string>("[\"auto-local::large-model\"]"));

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute local path rejects unknown forced model", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {holder::llm::LocalModel{.name = "installed-model", .size = 1}};
  runner.set_status_override_for_tests(status);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket unopened_socket(ioc);

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{
      {"prompt", "force local"},
      {"project_id", "proj-1"},
      {"model", "missing-model"},
  }
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      unopened_socket,
      db,
      nullptr,
      nullptr,
      &runner_registry,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed == false);
  REQUIRE(res.result() == http::status::bad_request);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("AiRunPostRoute cloud compaction records below_threshold reason", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> {
        if (model.id == "main-model") return std::string("cloud output");
        return std::string("summary output");
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_threshold.yaml";
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
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
    out << "  summary_refresh:\n";
    out << "    trigger_context_tokens: 5000\n";
    out << "    source_context_tokens: 256\n";
    out << "    response_tokens_budget: 64\n";
    out << "    max_summary_chars: 2048\n";
    out << "    min_interval_seconds: 0\n";
    out << "    min_delta_tokens: 0\n";
    out << "    force_refresh_tokens: 10000\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"}, {"card_body", "tiny"}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["reason"] == "below_threshold");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud failure records cooldown for selected model", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig&,
         const std::string&,
         const std::string&,
         std::string* error) -> std::optional<std::string> {
        if (error) *error = "forced cloud failure";
        return std::nullopt;
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_failure.yaml";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  {
    std::ofstream out(cloud_cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
    out << "    cooldown:\n";
    out << "      base_seconds: 30\n";
    out << "      cap_seconds: 900\n";
    out << "  provider_defaults:\n";
    out << "    switchyard:\n";
    out << "      provider: Switchyard\n";
    out << "      credential_key: switchyard\n";
    out << "      enabled: true\n";
    out << "      base_url: https://127.0.0.1:1\n";
    out << "      api_kind: generic_chat\n";
    out << "      auth_type: bearer_header\n";
    out << "      provider_cooldown:\n";
    out << "        base_seconds: 50\n";
    out << "        cap_seconds: 700\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider_id: switchyard\n";
    out << "        model_id: failing-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        model_cooldown:\n";
    out << "          base_seconds: 12\n";
    out << "          cap_seconds: 180\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"provider", "switchyard"},
                              {"model", "failing-model"}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_project("proj-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "failed");
  REQUIRE(runs[0].error.has_value());
  REQUIRE(runs[0].error.value() == "forced cloud failure");
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["result"]["status"] == "failed");
  REQUIRE(trace["attempts"].is_array());
  REQUIRE_FALSE(trace["attempts"].empty());
  REQUIRE(trace["attempts"][0]["cooldown"]["remaining_seconds"].get<long long>() >= 0);

  const auto cooldown =
      holder::api::support::load_cloud_model_cooldown(db, "switchyard", "failing-model");
  REQUIRE(cooldown.has_value());
  REQUIRE(cooldown->failure_count == 1);
  REQUIRE(cooldown->last_error == "forced cloud failure");
  REQUIRE(cooldown->cooldown_until - cooldown->updated_at == 12);

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud path records attempt rejection reasons on failed run", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_attempt_reasons.yaml";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  {
    std::ofstream out(cloud_cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: model-cooldown\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: model-rpm\n";
    out << "        limits:\n";
    out << "          rpm: 1\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: model-rpd\n";
    out << "        limits:\n";
    out << "          rpd: 1\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: model-tpm\n";
    out << "        limits:\n";
    out << "          tpm: 100\n";
    out << "        endpoint: /api/v1/chat/completions\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  const long long now = holder::api::support::now_epoch_seconds();
  db.exec("INSERT INTO ai_cloud_model_cooldowns(provider, model_id, failure_count, cooldown_until, "
          "last_error, updated_at) VALUES('switchyard', 'model-cooldown', 2, 4102444800, "
          "'cooldown test', " +
          std::to_string(now) + ");");
  holder::api::support::record_cloud_usage_event(
      db, "switchyard", "model-rpm", 10, 5, now, "rpm-seed");
  holder::api::support::record_cloud_usage_event(
      db, "switchyard", "model-rpd", 10, 5, now, "rpd-seed");

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "make this fail but keep context"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"context", {{"card_id", "card-1"}, {"card_body", "context body"}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "failed");
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["result"]["status"] == "failed");
  REQUIRE(trace["attempts"].is_array());

  bool saw_cooldown = false;
  bool saw_rpm = false;
  bool saw_rpd = false;
  bool saw_tpm = false;
  for (const auto& attempt : trace["attempts"]) {
    if (!attempt.is_object() || !attempt.contains("reason")) continue;
    const auto reason = attempt["reason"].get<std::string>();
    if (reason == "cooldown_active") saw_cooldown = true;
    if (reason == "rpm_exceeded") saw_rpm = true;
    if (reason == "rpd_exceeded") saw_rpd = true;
    if (reason == "tpm_exceeded") saw_tpm = true;
  }
  REQUIRE(saw_cooldown);
  REQUIRE(saw_rpm);
  REQUIRE(saw_rpd);
  REQUIRE(saw_tpm);
  for (const auto& attempt : trace["attempts"]) {
    if (attempt.is_object() && attempt.value("reason", "") == "cooldown_active") {
      REQUIRE(attempt["cooldown"]["remaining_seconds"].get<long long>() > 0);
    }
  }

  const auto rolled = holder::api::support::load_thread_compaction_state(db, "thread-1");
  REQUIRE(rolled.has_value());

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud compaction records min_interval_not_elapsed reason", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> {
        if (model.id == "main-model") return std::string("cloud output");
        return std::string("summary output");
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_min_interval.yaml";
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
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
    out << "  summary_refresh:\n";
    out << "    trigger_context_tokens: 1\n";
    out << "    source_context_tokens: 256\n";
    out << "    response_tokens_budget: 64\n";
    out << "    max_summary_chars: 2048\n";
    out << "    min_interval_seconds: 86400\n";
    out << "    min_delta_tokens: 0\n";
    out << "    force_refresh_tokens: 999999\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);
  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-1";
  state.rolling_summary = std::string("existing summary");
  state.updated_at = holder::api::support::now_epoch_seconds();
  holder::api::support::upsert_thread_compaction_state(db, state);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  const std::string long_context(6000, 'x');
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"},
                                           {"card_body", long_context}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["reason"] == "min_interval_not_elapsed");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud compaction records min_delta_not_met reason", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> {
        if (model.id == "main-model") return std::string("cloud output");
        return std::string("summary output");
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_min_delta.yaml";
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
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
    out << "  summary_refresh:\n";
    out << "    trigger_context_tokens: 1\n";
    out << "    source_context_tokens: 256\n";
    out << "    response_tokens_budget: 64\n";
    out << "    max_summary_chars: 2048\n";
    out << "    min_interval_seconds: 0\n";
    out << "    min_delta_tokens: 999999\n";
    out << "    force_refresh_tokens: 999999\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  holder::api::support::ThreadCompactionState state;
  state.thread_id = "thread-1";
  state.rolling_summary = std::string(5000, 's');
  state.updated_at = 1;
  holder::api::support::upsert_thread_compaction_state(db, state);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  const std::string long_context_delta(6000, 'd');
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"}, {"card_body", long_context_delta}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["reason"] == "min_delta_not_met");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud compaction records cooldown_active reason", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> {
        if (model.id == "main-model") return std::string("cloud output");
        return std::string("summary output");
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_compact_cooldown.yaml";
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
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
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
    out << "    force_refresh_tokens: 999999\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  db.exec("INSERT INTO ai_cloud_model_cooldowns(provider, model_id, failure_count, cooldown_until, "
          "last_error, updated_at) VALUES('switchyard', 'compact-model', 2, 4102444800, "
          "'summary cooldown', 1);");

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  const std::string long_context_cooldown(6000, 'c');
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"}, {"card_body", long_context_cooldown}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["reason"] == "cooldown_active");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud compaction refresh completes and stores normalized summary", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> {
        if (model.id == "compact-model") {
          return std::string("## Decisions\n"
                             "- Keep cloud-first fallback for low-end devices\n"
                             "## Constraints\n"
                             "- Avoid user editing yaml files\n"
                             "## Open Questions\n"
                             "- Should provider order be user-customizable?\n"
                             "## Next Actions\n"
                             "- Add client-facing bootstrap endpoint\n");
        }
        return std::string("cloud output");
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_refresh_complete.yaml";
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
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
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
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);
  holder::api::support::ThreadCompactionState prior_state;
  prior_state.thread_id = "thread-1";
  prior_state.rolling_summary = std::string("prior summary");
  prior_state.updated_at = 1;
  holder::api::support::upsert_thread_compaction_state(db, prior_state);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  const std::string long_context_complete(6000, 'z');
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"},
                                           {"card_body", long_context_complete}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["status"] == "completed");
  REQUIRE(trace["compaction"]["summary_refresh"]["model"] == "compact-model");

  const auto state = holder::api::support::load_thread_compaction_state(db, "thread-1");
  REQUIRE(state.has_value());
  REQUIRE(state->rolling_summary.has_value());
  REQUIRE(state->rolling_summary->find("## Decisions") != std::string::npos);

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud compaction summary refresh rejects rpm limit", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig&,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> { return std::string("cloud output"); });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_compact_rpm.yaml";
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
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
    out << "        limits:\n";
    out << "          rpm: 1\n";
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
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);
  const auto now = holder::api::support::now_epoch_seconds();
  holder::api::support::record_cloud_usage_event(
      db, "switchyard", "compact-model", 10, 5, now, "compact-rpm");

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  const std::string long_context_rpm(6000, 'r');
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"}, {"card_body", long_context_rpm}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req, res, server_socket, db, nullptr, secret_store.get(), nullptr, [&id_seq]() {
        return std::string("uuid-") + std::to_string(id_seq++);
      });
  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["reason"] == "rpm_exceeded");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute local write-header failure returns early", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {holder::llm::LocalModel{.name = "installed-model", .size = 1}};
  runner.set_status_override_for_tests(status);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket unopened_socket(ioc);

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "local prompt"}, {"project_id", "proj-1"}}.dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req, res, unopened_socket, db, nullptr, secret_store.get(), &runner_registry, [&id_seq]() {
        return std::string("uuid-") + std::to_string(id_seq++);
      });
  REQUIRE(out.handled);
  REQUIRE(out.streamed);
}

TEST_CASE("AiRunPostRoute local path marks run failed when all models fail", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {
      holder::llm::LocalModel{.name = "router-model", .size = 1},
      holder::llm::LocalModel{.name = "model-a", .size = 10},
      holder::llm::LocalModel{.name = "model-b", .size = 20},
  };
  runner.set_status_override_for_tests(status);
  runner.set_stream_generate_override_for_tests(
      [&](const std::string& model,
          const std::string&,
          const std::string&,
          const std::function<void(const std::string&)>&,
          std::string* error) -> bool {
        if (model == "router-model") return true;
        if (error) *error = "forced failure";
        return false;
      });

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "local prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req, res, server_socket, db, nullptr, secret_store.get(), &runner_registry, [&id_seq]() {
        return std::string("uuid-") + std::to_string(id_seq++);
      });
  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "failed");
  REQUIRE(runs[0].error == std::optional<std::string>("no output"));

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute local routing uses configured fast model and category metadata", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  const auto ai_catalog_path = dir / "ai_catalog_local_meta.yaml";
  std::filesystem::create_directories(repo_dir);
  {
    std::ofstream out(ai_catalog_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  Models:\n";
    out << "    Local:\n";
    out << "      - tag: model-a\n";
    out << "        category: coder\n";
    out << "      - tag: model-b\n";
    out << "        category: general\n";
  }
  holder::test::EnvGuard ai_catalog_env("HOLDER_AI_CATALOG_PATH", ai_catalog_path.string());

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  holder::ai::AiLocalModelConfigRepo local_model_cfg_repo(db);
  local_model_cfg_repo.set(std::string("router-model"), std::nullopt, std::nullopt, 1);

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {
      holder::llm::LocalModel{.name = "router-model", .size = 1},
      holder::llm::LocalModel{.name = "model-a", .size = 100},
      holder::llm::LocalModel{.name = "model-b", .size = 200},
  };
  runner.set_status_override_for_tests(status);
  bool router_used_project_cfg = false;
  bool saw_category_in_router_prompt = false;
  runner.set_stream_generate_override_for_tests(
      [&](const std::string& model,
          const std::string& prompt,
          const std::string&,
          const std::function<void(const std::string&)>& on_chunk,
          std::string*) -> bool {
        if (model == "router-model") {
          router_used_project_cfg = true;
          if (prompt.find(", category=coder") != std::string::npos) {
            saw_category_in_router_prompt = true;
          }
          on_chunk("[\"model-a\"]");
          return true;
        }
        if (model == "model-a") {
          on_chunk("model-a-output");
          return true;
        }
        return false;
      });

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "route via project cfg"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req, res, server_socket, db, nullptr, secret_store.get(), &runner_registry, [&id_seq]() {
        return std::string("uuid-") + std::to_string(id_seq++);
      });
  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  REQUIRE(router_used_project_cfg);
  REQUIRE(saw_category_in_router_prompt);

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute local routing catches router config repo errors and falls back", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  db.exec("DROP TABLE ai_local_model_config;");

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {
      holder::llm::LocalModel{.name = "router-model", .size = 1},
      holder::llm::LocalModel{.name = "model-a", .size = 50},
  };
  runner.set_status_override_for_tests(status);
  runner.set_stream_generate_override_for_tests(
      [&](const std::string& model,
          const std::string&,
          const std::string&,
          const std::function<void(const std::string&)>& on_chunk,
          std::string*) -> bool {
        if (model == "router-model") {
          on_chunk("[\"model-a\"]");
          return true;
        }
        if (model == "model-a") {
          on_chunk("ok");
          return true;
        }
        return false;
      });

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "fallback when cfg repo throws"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req, res, server_socket, db, nullptr, secret_store.get(), &runner_registry, [&id_seq]() {
        return std::string("uuid-") + std::to_string(id_seq++);
      });
  REQUIRE(out.handled);
  REQUIRE(out.streamed);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "completed");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute local replies use configured strong model", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  holder::ai::AiLocalModelConfigRepo local_model_cfg_repo(db);
  local_model_cfg_repo.set(std::string("fast-model"), std::string("strong-model"), std::nullopt, 1);

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {
      holder::llm::LocalModel{.name = "fast-model", .size = 1},
      holder::llm::LocalModel{.name = "strong-model", .size = 500},
      holder::llm::LocalModel{.name = "other-model", .size = 200},
  };
  runner.set_status_override_for_tests(status);

  bool saw_fast_model = false;
  bool saw_strong_model = false;
  runner.set_stream_generate_override_for_tests(
      [&](const std::string& model,
          const std::string&,
          const std::string&,
          const std::function<void(const std::string&)>& on_chunk,
          std::string*) -> bool {
        if (model == "fast-model") {
          saw_fast_model = true;
          on_chunk("[\"other-model\"]");
          return true;
        }
        if (model == "strong-model") {
          saw_strong_model = true;
          on_chunk("strong-output");
          return true;
        }
        return false;
      });

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "use strong model"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req, res, server_socket, db, nullptr, secret_store.get(), &runner_registry, [&id_seq]() {
        return std::string("uuid-") + std::to_string(id_seq++);
      });
  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  REQUIRE_FALSE(saw_fast_model);
  REQUIRE(saw_strong_model);

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].chosen_model == std::optional<std::string>("auto-local::strong-model"));

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute title generation honors configured fast model runner", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  holder::test::EnvGuard fake_env("HOLDER_MODEL_RUNNER_FAKE", "1");
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");
  holder::ai::AiRunnerRepo(db).upsert(holder::model::AiRunner{
      .runner_id = "manual-a",
      .name = "Office Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://office:11434"),
      .source = "manual",
      .enabled = true,
      .created_at = 1,
      .updated_at = 1,
  });
  holder::ai::AiLocalModelConfigRepo local_model_cfg_repo(db);
  local_model_cfg_repo.set(
      std::string("manual-a::fake-echo"), std::string("auto-local::strong-model"), std::nullopt, 1);

  holder::llm::LocalModelRunner runner;
  runner.set_fake_mode(false);
  holder::llm::RunnerStatus status;
  status.available = true;
  status.models = {
      holder::llm::LocalModel{.name = "fast-model", .size = 1},
      holder::llm::LocalModel{.name = "strong-model", .size = 500},
      holder::llm::LocalModel{.name = "other-model", .size = 200},
  };
  runner.set_status_override_for_tests(status);

  bool saw_main_generation = false;
  bool saw_title_generation_on_auto_local = false;
  runner.set_stream_generate_override_for_tests(
      [&](const std::string& model,
          const std::string& prompt,
          const std::string&,
          const std::function<void(const std::string&)>& on_chunk,
          std::string*) -> bool {
        if (prompt.find("Write a short human-readable thread title") != std::string::npos) {
          saw_title_generation_on_auto_local = true;
          on_chunk("Auto Local Title");
          return true;
        }
        if (model == "strong-model") {
          saw_main_generation = true;
          on_chunk("strong-output");
          return true;
        }
        return false;
      });

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  req.body() = nlohmann::json{{"prompt", "use strong model"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  auto* manual_client = runner_registry.get_client("manual-a");
  REQUIRE(manual_client != nullptr);
  (void)manual_client->retry();

  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req, res, server_socket, db, nullptr, secret_store.get(), &runner_registry, [&id_seq]() {
        return std::string("uuid-") + std::to_string(id_seq++);
      });
  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  REQUIRE(saw_main_generation);
  REQUIRE_FALSE(saw_title_generation_on_auto_local);

  holder::ai::AiThreadRepo thread_repo(db);
  const auto thread = thread_repo.get("thread-1");
  REQUIRE(thread.has_value());
  REQUIRE(thread->title == "Thread");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud compaction records quality_guard_failed reason", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string*) -> std::optional<std::string> {
        if (model.id == "compact-model") {
          return std::string("ok");
        }
        return std::string("cloud output");
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_quality_guard.yaml";
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
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider: Switchyard\n";
    out << "        provider_id: switchyard\n";
    out << "        credential_key: switchyard\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_chat\n";
    out << "        auth_type: bearer_header\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
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
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  const std::string long_context_quality(6000, 'q');
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"},
                                           {"card_body", long_context_quality}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["reason"] == "quality_guard_failed");

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("AiRunPostRoute cloud compaction records failed summary refresh cooldown", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig&,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string* error) -> std::optional<std::string> {
        if (model.id == "compact-model") {
          if (error) *error = "summary refresh failed (forced)";
          return std::nullopt;
        }
        return std::string("cloud output");
      });

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog_summary_fail.yaml";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);

  {
    std::ofstream out(cloud_cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
    out << "    cooldown:\n";
    out << "      base_seconds: 30\n";
    out << "      cap_seconds: 900\n";
    out << "  provider_defaults:\n";
    out << "    switchyard:\n";
    out << "      provider: Switchyard\n";
    out << "      credential_key: switchyard\n";
    out << "      enabled: true\n";
    out << "      base_url: https://127.0.0.1:1\n";
    out << "      api_kind: generic_chat\n";
    out << "      auth_type: bearer_header\n";
    out << "      provider_cooldown:\n";
    out << "        base_seconds: 50\n";
    out << "        cap_seconds: 700\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider_id: switchyard\n";
    out << "        model_id: main-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "      - provider_id: switchyard\n";
    out << "        model_id: compact-model\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: compact\n";
    out << "        model_cooldown:\n";
    out << "          base_seconds: 7\n";
    out << "          cap_seconds: 70\n";
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
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;
  boost::asio::io_context ioc;
  tcp::socket client(ioc);
  tcp::socket server_socket(ioc);
  try {
    tcp::acceptor acceptor(ioc, {boost::asio::ip::make_address("127.0.0.1"), 0});
    const auto endpoint = acceptor.local_endpoint();
    client.connect(endpoint);
    acceptor.accept(server_socket);
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket pair not available in test environment: ") + ex.what());
  }

  http::request<http::string_body> req{http::verb::post, "/ai/runs", 11};
  req.set(http::field::host, "127.0.0.1");
  req.set(http::field::content_type, "application/json");
  const std::string long_context_summary_fail(6000, 'f');
  req.body() = nlohmann::json{{"prompt", "cloud prompt"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"provider", "switchyard"},
                              {"model", "main-model"},
                              {"context", {{"card_id", "card-1"},
                                           {"card_body", long_context_summary_fail}}}}
                   .dump();
  req.prepare_payload();

  http::response<http::string_body> res;
  int id_seq = 1;
  const auto out = holder::api::routes::ai::runs::handle_ai_runs_post_route(
      req,
      res,
      server_socket,
      db,
      nullptr,
      secret_store.get(),
      nullptr,
      [&id_seq]() { return std::string("uuid-") + std::to_string(id_seq++); });

  REQUIRE(out.handled);
  REQUIRE(out.streamed);
  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(trace["compaction"]["summary_refresh"]["status"] == "failed");
  REQUIRE(trace["compaction"]["summary_refresh"]["error"] == "summary refresh failed (forced)");

  const auto cooldown =
      holder::api::support::load_cloud_model_cooldown(db, "switchyard", "compact-model");
  REQUIRE(cooldown.has_value());
  REQUIRE(cooldown->failure_count == 1);
  REQUIRE(cooldown->last_error == "summary refresh failed (forced)");
  REQUIRE(cooldown->cooldown_until - cooldown->updated_at == 7);

  boost::system::error_code ec;
  server_socket.shutdown(tcp::socket::shutdown_both, ec);
  server_socket.close(ec);
  client.shutdown(tcp::socket::shutdown_both, ec);
  client.close(ec);
}

TEST_CASE("HTTP ai runs post stores run and messages", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);

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
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);

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
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);
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
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  const std::string long_prompt(140, 'x');
  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);
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
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);
  holder::ai::AiProviderSettingRepo setting_repo(db);
  setting_repo.upsert("switchyard", false, 2);

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);
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
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);
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
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);
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
    if (m.model.has_value() && m.model.value() == "auto-local::fake-echo") {
      saw_model = true;
    }
  }
  REQUIRE(saw_user);
  REQUIRE(saw_model);
}

TEST_CASE("HTTP ai runs local path rejects forced model that is not installed", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::LocalModelRunner runner;
  runner.start_background_probe();
  holder::llm::LocalRunnerClient local_runner_client(&runner);
  holder::llm::RunnerRegistry runner_registry(&db, &local_runner_client);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);
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

TEST_CASE("HTTP ai runs can target a manual runner by runner_id", "[http]") {
  holder::test::EnvGuard fake_runner("HOLDER_MODEL_RUNNER_FAKE", "1");

  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  const auto db_path = dir / "holder.db";
  const auto repo_dir = dir / "repo";
  std::filesystem::create_directories(repo_dir);
  holder::platform::Db db = holder::test::open_db_with_schema(db_path);
  db.exec(std::string("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
                      "VALUES('proj-1', 'Project', '") +
          repo_dir.string() + "', 1, 1);");
  db.exec("INSERT INTO ai_threads(thread_id, project_id, title, created_at, updated_at) "
          "VALUES('thread-1', 'proj-1', 'Thread', 1, 1);");

  holder::ai::AiRunnerRepo runner_repo(db);
  runner_repo.upsert(holder::model::AiRunner{
      .runner_id = "manual-a",
      .name = "Office Ollama",
      .kind = "ollama",
      .base_url = std::optional<std::string>("http://office:11434"),
      .source = "manual",
      .enabled = true,
      .created_at = 1,
      .updated_at = 1,
  });

  const std::string token = "testtoken";
  holder::card::CardStore card_store(db, nullptr);
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, &card_store, nullptr, nullptr, &runner_registry);
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
  req.body() = nlohmann::json{{"prompt", "hello manual"},
                              {"project_id", "proj-1"},
                              {"thread_id", "thread-1"},
                              {"runner_id", "manual-a"},
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

  REQUIRE(res.body().find("\"runner_id\":\"manual-a\"") != std::string::npos);
  REQUIRE(res.body().find("\"model_ref\":\"manual-a::fake-echo\"") != std::string::npos);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_thread("thread-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "completed");
  REQUIRE(runs[0].chosen_model == std::optional<std::string>("manual-a::fake-echo"));
}

TEST_CASE("HTTP ai runs cloud path returns not configured when no enabled provider has creds", "[http]") {
  const auto dir = make_temp_dir();
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
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
