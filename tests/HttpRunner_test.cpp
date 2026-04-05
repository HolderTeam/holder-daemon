#include "http_test_helpers.h"
#include "api/support/CloudClient.h"
#include "ai/AiProviderCredentialRepo.h"
#include "ai/AiRunRepo.h"
#include "privacy/SecretStore.h"

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

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

TEST_CASE("HTTP ai capabilities returns not configured when runtime missing", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);

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

  const auto caps = http_json_request(bound.bind,
                                      bound.port,
                                      token,
                                      boost::beast::http::verb::get,
                                      "/ai/capabilities",
                                      nlohmann::json{},
                                      boost::beast::http::status::ok);
  REQUIRE(caps["ok"] == true);
  REQUIRE(caps["data"]["runner_available"] == false);
  REQUIRE(caps["data"]["error"].is_string());
  REQUIRE(caps["data"]["models"].is_array());
  REQUIRE(caps["data"]["recommended_models"].is_array());
  REQUIRE(caps["data"]["recommended_install"].is_array());
  REQUIRE(caps["data"].contains("caste"));
  REQUIRE(caps["data"].contains("local_model_config"));
  REQUIRE(caps["data"]["local_model_config"]["fast_model"].is_null());
  REQUIRE(caps["data"]["local_model_config"]["strong_model"].is_null());
  REQUIRE(caps["data"]["local_model_config"]["deep_model"].is_null());

  const auto status = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::get,
                                        "/ai/status",
                                        nlohmann::json{},
                                        boost::beast::http::status::ok);
  REQUIRE(status["ok"] == true);
  REQUIRE(status["data"]["runner_available"] == false);
  REQUIRE(status["data"]["active_runs"].is_number_integer());
  REQUIRE(status["data"]["active_pull_jobs"].is_number_integer());
  REQUIRE(status["data"]["pulls"].is_array());

  const auto retry = http_json_request(bound.bind,
                                       bound.port,
                                       token,
                                       boost::beast::http::verb::post,
                                       "/ai/runners/auto-local/retry",
                                       nlohmann::json{},
                                       boost::beast::http::status::not_found);
  REQUIRE(retry["ok"] == false);
  REQUIRE(retry["error"]["code"] == "not_found");

  const auto pull = http_json_request(bound.bind,
                                      bound.port,
                                      token,
                                      boost::beast::http::verb::post,
                                      "/ai/runner/pull",
                                      nlohmann::json{{"model", "qwen2.5:0.5b"}},
                                      boost::beast::http::status::not_found);
  REQUIRE(pull["ok"] == false);
  REQUIRE(pull["error"]["code"] == "not_found");

  const auto pull_status = http_json_request(bound.bind,
                                             bound.port,
                                             token,
                                             boost::beast::http::verb::get,
                                             "/ai/runner/pull/nonexistent",
                                             nlohmann::json{},
                                             boost::beast::http::status::not_found);
  REQUIRE(pull_status["ok"] == false);
  REQUIRE(pull_status["error"]["code"] == "not_found");

  const auto complete = http_json_request(bound.bind,
                                          bound.port,
                                          token,
                                          boost::beast::http::verb::post,
                                          "/ai/runs",
                                          nlohmann::json{{"prompt", "hello"}},
                                          boost::beast::http::status::service_unavailable);
  REQUIRE(complete["ok"] == false);
  REQUIRE((complete["error"]["code"] == "runner_unavailable" ||
           complete["error"]["code"] == "cloud_not_configured"));

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runners supports CRUD for manual runners", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);

  const std::string token = "testtoken";
  holder::llm::RunnerRegistry runner_registry(&db, nullptr);
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr, nullptr, &runner_registry);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() { server.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto list = http_json_request(bound.bind,
                                bound.port,
                                token,
                                boost::beast::http::verb::get,
                                "/ai/runners",
                                nlohmann::json{},
                                boost::beast::http::status::ok);
  REQUIRE(list["ok"] == true);
  REQUIRE(list["data"]["runners"].is_array());
  REQUIRE(list["data"]["runners"].size() == 1);
  REQUIRE(list["data"]["runners"][0]["runner_id"] == "auto-local");

  auto created = http_json_request(bound.bind,
                                   bound.port,
                                   token,
                                   boost::beast::http::verb::post,
                                   "/ai/runners",
                                   nlohmann::json{
                                       {"name", "Office Ollama"},
                                       {"kind", "ollama"},
                                       {"base_url", "http://office:11434"},
                                   },
                                   boost::beast::http::status::created);
  REQUIRE(created["ok"] == true);
  const std::string runner_id = created["data"]["runner_id"].get<std::string>();
  REQUIRE(runner_id.rfind("manual-", 0) == 0);
  REQUIRE(created["data"]["runtime"]["configured"] == true);

  auto fetched = http_json_request(bound.bind,
                                   bound.port,
                                   token,
                                   boost::beast::http::verb::get,
                                   "/ai/runners/" + runner_id,
                                   nlohmann::json{},
                                   boost::beast::http::status::ok);
  REQUIRE(fetched["data"]["runner_id"] == runner_id);
  REQUIRE(fetched["data"]["name"] == "Office Ollama");

  auto patched = http_json_request(bound.bind,
                                   bound.port,
                                   token,
                                   boost::beast::http::verb::patch,
                                   "/ai/runners/" + runner_id,
                                   nlohmann::json{{"enabled", false}, {"name", "Desk Ollama"}},
                                   boost::beast::http::status::ok);
  REQUIRE(patched["data"]["enabled"] == false);
  REQUIRE(patched["data"]["name"] == "Desk Ollama");
  REQUIRE(patched["data"]["runtime"]["configured"] == false);

  auto deleted = http_json_request(bound.bind,
                                   bound.port,
                                   token,
                                   boost::beast::http::verb::delete_,
                                   "/ai/runners/" + runner_id,
                                   nlohmann::json{},
                                   boost::beast::http::status::ok);
  REQUIRE(deleted["data"]["runner_id"] == runner_id);

  auto missing = http_json_request(bound.bind,
                                   bound.port,
                                   token,
                                   boost::beast::http::verb::get,
                                   "/ai/runners/" + runner_id,
                                   nlohmann::json{},
                                   boost::beast::http::status::not_found);
  REQUIRE(missing["ok"] == false);
  REQUIRE(missing["error"]["code"] == "not_found");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs cloud fallback selects switchyard when configured", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig& provider,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string* error) -> std::optional<std::string> {
        if (provider.id != "switchyard") {
          if (error) *error = "unexpected provider";
          return std::nullopt;
        }
        if (model.id != "openrouter/auto") {
          if (error) *error = "unexpected model";
          return std::nullopt;
        }
        return std::string("mock switchyard output");
      });

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog.yaml";
  holder::test::EnvGuard xdg_data_home("XDG_DATA_HOME", (dir / "xdg-data").string());
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

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
    out << "        header_name: Authorization\n";
    out << "        bearer_prefix: Bearer\n";
    out << "        model_id: openrouter/auto\n";
    out << "        endpoint: /api/v1/chat/completions\n";
    out << "        role: default\n";
    out << "        limits:\n";
    out << "          rpm: 0\n";
    out << "          tpm: 0\n";
    out << "          rpd: 0\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: switchyard\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  auto db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  seed_provider_credential(db, *secret_store, "switchyard", "test-key", 1, 1);

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

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));
  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  nlohmann::json body = {
      {"prompt", "hello from switchyard test"},
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
  REQUIRE(res.result() == http::status::ok);
  REQUIRE(res.body().find("\"provider\":\"switchyard\"") != std::string::npos);
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
  REQUIRE(policy_trace["result"]["status"] == "completed");
  REQUIRE(policy_trace["result"]["model"] == "openrouter/auto");
  REQUIRE(policy_trace["attempts"].is_array());
  REQUIRE_FALSE(policy_trace["attempts"].empty());
  REQUIRE(policy_trace["attempts"][0]["model"] == "openrouter/auto");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs cloud fallback selects chadjeopardy when configured", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig& provider,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string* error) -> std::optional<std::string> {
        if (provider.id != "chadjeopardy") {
          if (error) *error = "unexpected provider";
          return std::nullopt;
        }
        if (model.id != "gpt-5.2") {
          if (error) *error = "unexpected model";
          return std::nullopt;
        }
        return std::string("mock chadjeopardy output");
      });

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog.yaml";
  holder::test::EnvGuard xdg_data_home("XDG_DATA_HOME", (dir / "xdg-data").string());
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

  {
    std::ofstream out(cloud_cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider: ChadJeopardy\n";
    out << "        provider_id: chadjeopardy\n";
    out << "        credential_key: chadjeopardy\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: generic_responses\n";
    out << "        auth_type: bearer_header\n";
    out << "        header_name: Authorization\n";
    out << "        bearer_prefix: Bearer\n";
    out << "        model_id: gpt-5.2\n";
    out << "        endpoint: /v1/responses\n";
    out << "        role: default\n";
    out << "        limits:\n";
    out << "          rpm: 0\n";
    out << "          tpm: 0\n";
    out << "          rpd: 0\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: chadjeopardy\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  auto db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  seed_provider_credential(db, *secret_store, "chadjeopardy", "test-key", 1, 1);

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

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));
  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  nlohmann::json body = {
      {"prompt", "hello from chadjeopardy test"},
      {"project_id", "proj-1"},
      {"provider", "chadjeopardy"},
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
  REQUIRE(res.body().find("\"provider\":\"chadjeopardy\"") != std::string::npos);
  socket.shutdown(tcp::socket::shutdown_both);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_project("proj-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "completed");
  REQUIRE(runs[0].chosen_model == std::optional<std::string>("chadjeopardy:gpt-5.2"));
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto policy_trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(policy_trace["path"] == "cloud");
  REQUIRE(policy_trace["provider"] == "chadjeopardy");
  REQUIRE(policy_trace["result"]["status"] == "completed");
  REQUIRE(policy_trace["result"]["model"] == "gpt-5.2");
  REQUIRE(policy_trace["attempts"].is_array());
  REQUIRE_FALSE(policy_trace["attempts"].empty());
  REQUIRE(policy_trace["attempts"][0]["model"] == "gpt-5.2");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP ai runs cloud fallback selects mechatropic when configured", "[http]") {
  CloudRunOverrideGuard cloud_guard(
      [](const holder::api::support::CloudProviderConfig& provider,
         const holder::api::support::CloudModelConfig& model,
         const std::string&,
         const std::string&,
         std::string* error) -> std::optional<std::string> {
        if (provider.id != "mechatropic") {
          if (error) *error = "unexpected provider";
          return std::nullopt;
        }
        if (model.id != "claude-opus-4-6") {
          if (error) *error = "unexpected model";
          return std::nullopt;
        }
        return std::string("mock mechatropic output");
      });

  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  const auto cloud_cfg_path = dir / "ai_catalog.yaml";
  holder::test::EnvGuard xdg_data_home("XDG_DATA_HOME", (dir / "xdg-data").string());
  holder::test::EnvGuard keystore_dir("HOLDER_TEST_KEYSTORE_DIR", (dir / "keystore").string());

  {
    std::ofstream out(cloud_cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider: Mechatropic\n";
    out << "        provider_id: mechatropic\n";
    out << "        credential_key: mechatropic\n";
    out << "        enabled: true\n";
    out << "        base_url: https://127.0.0.1:1\n";
    out << "        api_kind: mechatropic_messages\n";
    out << "        auth_type: header_key\n";
    out << "        header_name: x-api-key\n";
    out << "        model_id: claude-opus-4-6\n";
    out << "        endpoint: /v1/messages\n";
    out << "        role: default\n";
    out << "        limits:\n";
    out << "          rpm: 0\n";
    out << "          tpm: 0\n";
    out << "          rpd: 0\n";
    out << "  runtime:\n";
    out << "    route_policy:\n";
    out << "      default_provider: mechatropic\n";
  }
  holder::test::EnvGuard cloud_cfg_env("HOLDER_AI_CATALOG_PATH", cloud_cfg_path.string());

  auto db = open_db_with_schema(db_path);
  db.exec("INSERT INTO projects(project_id, name, root_path, created_at, updated_at) "
          "VALUES('proj-1', 'Project', '/tmp/project', 1, 1);");
  auto secret_store = holder::privacy::make_default_secret_store(dir / "server");
  seed_provider_credential(db, *secret_store, "mechatropic", "test-key", 1, 1);

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

  namespace http = boost::beast::http;
  using tcp = boost::asio::ip::tcp;

  boost::asio::io_context ioc;
  tcp::resolver resolver(ioc);
  auto endpoints = resolver.resolve(bound.bind, std::to_string(bound.port));
  tcp::socket socket(ioc);
  boost::asio::connect(socket, endpoints);

  nlohmann::json body = {
      {"prompt", "hello from mechatropic test"},
      {"project_id", "proj-1"},
      {"provider", "mechatropic"},
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
  REQUIRE(res.body().find("\"provider\":\"mechatropic\"") != std::string::npos);
  socket.shutdown(tcp::socket::shutdown_both);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  holder::ai::AiRunRepo run_repo(db);
  const auto runs = run_repo.list_by_project("proj-1");
  REQUIRE(runs.size() == 1);
  REQUIRE(runs[0].status == "completed");
  REQUIRE(runs[0].chosen_model == std::optional<std::string>("mechatropic:claude-opus-4-6"));
  REQUIRE(runs[0].policy_trace_json.has_value());
  const auto policy_trace = nlohmann::json::parse(runs[0].policy_trace_json.value());
  REQUIRE(policy_trace["path"] == "cloud");
  REQUIRE(policy_trace["provider"] == "mechatropic");
  REQUIRE(policy_trace["result"]["status"] == "completed");
  REQUIRE(policy_trace["result"]["model"] == "claude-opus-4-6");
  REQUIRE(policy_trace["attempts"].is_array());
  REQUIRE_FALSE(policy_trace["attempts"].empty());
  REQUIRE(policy_trace["attempts"][0]["model"] == "claude-opus-4-6");

  std::raise(SIGTERM);
  server_thread.join();
}
