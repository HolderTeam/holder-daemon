#include "http_test_helpers.h"
#include "api/routes/ai/providers/AiProviderCatalogRoutes.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_req(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

class CwdGuard {
public:
  explicit CwdGuard(const std::filesystem::path& next) : prev_(std::filesystem::current_path()) {
    std::filesystem::current_path(next);
  }
  ~CwdGuard() { std::filesystem::current_path(prev_); }

private:
  std::filesystem::path prev_;
};
} // namespace

TEST_CASE("HTTP ai provider catalog reflects configured credentials", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";

  auto db = open_db_with_schema(db_path);
  const auto cloudproviders_path = std::filesystem::path(SCHEMA_SQL_PATH).parent_path().parent_path() /
                                   "config" / "ai_catalog.yaml";
  holder::test::EnvGuard cloudproviders_env("HOLDER_AI_CATALOG_PATH", cloudproviders_path.string());

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

  const auto before = http_json_request(bound.bind,
                                        bound.port,
                                        token,
                                        boost::beast::http::verb::get,
                                        "/ai/providers/catalog",
                                        nlohmann::json{},
                                        boost::beast::http::status::ok);
  REQUIRE(before["ok"] == true);
  REQUIRE(before["data"]["providers"].is_array());

  bool found_chocolatefactory = false;
  for (const auto& provider : before["data"]["providers"]) {
    if (provider["id"] == "chocolatefactory") {
      found_chocolatefactory = true;
      REQUIRE(provider["configured"] == false);
      REQUIRE(provider["setup_url"] == "https://aistudio.google.com/apikey");
      REQUIRE(provider["docs_url"] == "https://ai.google.dev/gemini-api/docs");
      REQUIRE(provider["api_key_label"] == "ChocolateFactory API key");
    }
  }
  REQUIRE(found_chocolatefactory);

  const auto put = http_json_request(bound.bind,
                                     bound.port,
                                     token,
                                     boost::beast::http::verb::put,
                                     "/ai/providers/credentials",
                                     nlohmann::json{{"provider", "chocolatefactory"},
                                                    {"api_key", "cf_test_key_abc"}},
                                     boost::beast::http::status::ok);
  REQUIRE(put["ok"] == true);

  const auto after = http_json_request(bound.bind,
                                       bound.port,
                                       token,
                                       boost::beast::http::verb::get,
                                       "/ai/providers/catalog",
                                       nlohmann::json{},
                                       boost::beast::http::status::ok);
  REQUIRE(after["ok"] == true);
  bool configured_chocolatefactory = false;
  for (const auto& provider : after["data"]["providers"]) {
    if (provider["id"] == "chocolatefactory") {
      configured_chocolatefactory = provider["configured"].get<bool>();
      REQUIRE(provider["auth"].is_object());
      REQUIRE_FALSE(provider["auth"].contains("credential_provider_key"));
    }
  }
  REQUIRE(configured_chocolatefactory);

  bool switchyard_enabled_before = true;
  for (const auto& provider : before["data"]["providers"]) {
    if (provider["id"] == "switchyard") {
      switchyard_enabled_before = provider["enabled"].get<bool>();
    }
  }
  REQUIRE(switchyard_enabled_before == false);

  const auto put_switchyard = http_json_request(bound.bind,
                                                bound.port,
                                                token,
                                                boost::beast::http::verb::put,
                                                "/ai/providers/credentials",
                                                nlohmann::json{{"provider", "switchyard"},
                                                               {"api_key", "sw_test_key_abc"}},
                                                boost::beast::http::status::ok);
  REQUIRE(put_switchyard["ok"] == true);

  const auto after_switchyard = http_json_request(bound.bind,
                                                  bound.port,
                                                  token,
                                                  boost::beast::http::verb::get,
                                                  "/ai/providers/catalog",
                                                  nlohmann::json{},
                                                  boost::beast::http::status::ok);
  bool switchyard_configured = false;
  bool switchyard_enabled = false;
  for (const auto& provider : after_switchyard["data"]["providers"]) {
    if (provider["id"] == "switchyard") {
      switchyard_configured = provider["configured"].get<bool>();
      switchyard_enabled = provider["enabled"].get<bool>();
    }
  }
  REQUIRE(switchyard_configured == true);
  REQUIRE(switchyard_enabled == true);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("AiProviderCatalogRoutes direct handles missing catalog and db failures", "[http]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  http::response<http::string_body> res;

  SECTION("missing catalog path returns bad_request") {
    CwdGuard cwd(dir);
    holder::test::EnvGuard missing_catalog("HOLDER_AI_CATALOG_PATH", (dir / "does-not-exist.yaml").string());
    auto req = make_req(http::verb::get, "/ai/providers/catalog");
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_catalog_routes(
        "/ai/providers/catalog", req, res, db));
    REQUIRE(res.result() == http::status::bad_request);
    const auto payload = nlohmann::json::parse(res.body());
    REQUIRE(payload["error"]["message"] == "ai_catalog.yaml models runtime/catalog not found.");
  }

  SECTION("db failure on repo list is caught") {
    const auto ai_catalog_path = std::filesystem::path(SCHEMA_SQL_PATH).parent_path().parent_path() /
                                 "config" / "ai_catalog.yaml";
    holder::test::EnvGuard catalog_env("HOLDER_AI_CATALOG_PATH", ai_catalog_path.string());
    holder::platform::Db unopened_db;
    auto req = make_req(http::verb::get, "/ai/providers/catalog");
    REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_catalog_routes(
        "/ai/providers/catalog", req, res, unopened_db));
    REQUIRE(res.result() == http::status::bad_request);
  }
}

TEST_CASE("AiProviderCatalogRoutes direct supports minimal provider fields", "[http]") {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  const auto cfg_path = dir / "ai_catalog_minimal.yaml";
  {
    std::ofstream out(cfg_path);
    REQUIRE(out.is_open());
    out << "models:\n";
    out << "  Models:\n";
    out << "    Cloud:\n";
    out << "      - provider: Minimal\n";
    out << "        provider_id: minimal\n";
    out << "        credential_key: minimal\n";
    out << "        enabled: true\n";
    out << "        model_id: tiny\n";
    out << "        endpoint: /v1/chat\n";
  }
  holder::test::EnvGuard catalog_env("HOLDER_AI_CATALOG_PATH", cfg_path.string());

  http::response<http::string_body> res;
  auto req = make_req(http::verb::get, "/ai/providers/catalog");
  REQUIRE(holder::api::routes::ai::providers::handle_ai_provider_catalog_routes(
      "/ai/providers/catalog", req, res, db));
  REQUIRE(res.result() == http::status::ok);
  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == true);
  REQUIRE(payload["data"]["providers"].is_array());
  REQUIRE(payload["data"]["providers"].size() == 1);
  REQUIRE(payload["data"]["providers"][0]["id"] == "minimal");
  REQUIRE(payload["data"]["providers"][0]["api"].is_object());
  REQUIRE(payload["data"]["providers"][0]["auth"].is_object());
}
