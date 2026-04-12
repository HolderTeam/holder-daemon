#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/StaticRoutes.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace http = boost::beast::http;

class CwdGuard {
public:
  explicit CwdGuard(const std::filesystem::path& next) : prev_(std::filesystem::current_path()) {
    std::filesystem::current_path(next);
  }
  ~CwdGuard() { std::filesystem::current_path(prev_); }

private:
  std::filesystem::path prev_;
};

http::request<http::string_body> make_request(http::verb method, const std::string& target) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  return req;
}

} // namespace

TEST_CASE("StaticRoutes rejects non-GET methods", "[static-routes]") {
  auto req = make_request(http::verb::post, "/docs");
  http::response<http::string_body> res;
  REQUIRE(holder::api::routes::handle_static_routes("/docs", req, res));
  REQUIRE(res.result() == http::status::method_not_allowed);
}

TEST_CASE("StaticRoutes openapi not-found variants", "[static-routes]") {
  const auto dir = holder::test::make_temp_dir();
  CwdGuard cwd(dir);

  holder::test::EnvGuard openapi_env("HOLDER_OPENAPI_PATH", (dir / "missing-openapi.yaml").string());
  auto req = make_request(http::verb::get, "/openapi.yaml");
  http::response<http::string_body> res;
  REQUIRE(holder::api::routes::handle_static_routes("/openapi.yaml", req, res));
  REQUIRE(res.result() == http::status::not_found);

  const auto unreadable_openapi = dir / "openapi-unreadable.yaml";
  {
    std::ofstream out(unreadable_openapi);
    REQUIRE(out.is_open());
    out << "openapi: 3.0.0\n";
  }
  std::filesystem::permissions(unreadable_openapi,
                               std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);
  holder::test::EnvGuard openapi_env_dir("HOLDER_OPENAPI_PATH", unreadable_openapi.string());
  http::response<http::string_body> res2;
  REQUIRE(holder::api::routes::handle_static_routes("/openapi.yaml", req, res2));
  REQUIRE(res2.result() == http::status::not_found);
}

TEST_CASE("StaticRoutes serves docs and openapi success cases", "[static-routes]") {
  const auto dir = holder::test::make_temp_dir();
  CwdGuard cwd(dir);

  const auto docs_root = dir / "assets" / "swagger-ui";
  std::filesystem::create_directories(docs_root);
  {
    std::ofstream out(docs_root / "index.html");
    REQUIRE(out.is_open());
    out << "<!doctype html><title>Docs</title>";
  }

  const auto openapi_path = dir / "openapi.yaml";
  {
    std::ofstream out(openapi_path);
    REQUIRE(out.is_open());
    out << "openapi: 3.0.0\ninfo:\n  title: Test\n  version: 0.1\n";
  }

  holder::test::EnvGuard docs_env("HOLDER_DOCS_ROOT", docs_root.string());
  holder::test::EnvGuard openapi_env("HOLDER_OPENAPI_PATH", openapi_path.string());

  auto docs_req = make_request(http::verb::get, "/docs");
  http::response<http::string_body> docs_res;
  REQUIRE(holder::api::routes::handle_static_routes("/docs", docs_req, docs_res));
  REQUIRE(docs_res.result() == http::status::ok);
  REQUIRE(std::string(docs_res[http::field::content_type]).find("text/html") != std::string::npos);
  REQUIRE(docs_res.body().find("Docs") != std::string::npos);

  auto openapi_req = make_request(http::verb::get, "/openapi.yaml");
  http::response<http::string_body> openapi_res;
  REQUIRE(holder::api::routes::handle_static_routes("/openapi.yaml", openapi_req, openapi_res));
  REQUIRE(openapi_res.result() == http::status::ok);
  REQUIRE(std::string(openapi_res[http::field::content_type]).find("application/yaml") !=
          std::string::npos);
  REQUIRE(openapi_res.body().find("openapi:") != std::string::npos);
}

TEST_CASE("StaticRoutes ai_catalog json handles not-found and parse errors", "[static-routes]") {
  const auto dir = holder::test::make_temp_dir();
  CwdGuard cwd(dir);

  auto req = make_request(http::verb::get, "/ai_catalog.json");

  holder::test::EnvGuard missing_env("HOLDER_AI_CATALOG_PATH", (dir / "missing-ai.yaml").string());
  http::response<http::string_body> missing_res;
  REQUIRE(holder::api::routes::handle_static_routes("/ai_catalog.json", req, missing_res));
  REQUIRE(missing_res.result() == http::status::not_found);

  const auto ai_unreadable = dir / "ai-unreadable.yaml";
  {
    std::ofstream out(ai_unreadable);
    REQUIRE(out.is_open());
    out << "x: 1\n";
  }
  std::filesystem::permissions(ai_unreadable,
                               std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);
  holder::test::EnvGuard unreadable_env("HOLDER_AI_CATALOG_PATH", ai_unreadable.string());
  http::response<http::string_body> unreadable_res;
  REQUIRE(holder::api::routes::handle_static_routes("/ai_catalog.json", req, unreadable_res));
  REQUIRE(unreadable_res.result() == http::status::not_found);

  const auto bad_yaml = dir / "bad-ai.yaml";
  {
    std::ofstream out(bad_yaml);
    REQUIRE(out.is_open());
    out << "x: [1, 2\n";
  }
  holder::test::EnvGuard bad_env("HOLDER_AI_CATALOG_PATH", bad_yaml.string());
  http::response<http::string_body> bad_res;
  REQUIRE(holder::api::routes::handle_static_routes("/ai_catalog.json", req, bad_res));
  REQUIRE(bad_res.result() == http::status::internal_server_error);

  const auto null_yaml = dir / "null-ai.yaml";
  {
    std::ofstream out(null_yaml);
    REQUIRE(out.is_open());
  }
  holder::test::EnvGuard null_env("HOLDER_AI_CATALOG_PATH", null_yaml.string());
  http::response<http::string_body> null_res;
  REQUIRE(holder::api::routes::handle_static_routes("/ai_catalog.json", req, null_res));
  REQUIRE(null_res.result() == http::status::ok);
  REQUIRE(null_res.body().find("null") != std::string::npos);
}

TEST_CASE("StaticRoutes git_providers missing and parse errors", "[static-routes]") {
  const auto dir = holder::test::make_temp_dir();
  CwdGuard cwd(dir);

  auto yaml_req = make_request(http::verb::get, "/git_providers.yaml");
  auto json_req = make_request(http::verb::get, "/git_providers.json");

  holder::test::EnvGuard missing_env("HOLDER_GIT_PROVIDERS_PATH", (dir / "missing-git.yaml").string());
  http::response<http::string_body> miss_yaml_res;
  REQUIRE(holder::api::routes::handle_static_routes("/git_providers.yaml", yaml_req, miss_yaml_res));
  REQUIRE(miss_yaml_res.result() == http::status::not_found);
  http::response<http::string_body> miss_json_res;
  REQUIRE(holder::api::routes::handle_static_routes("/git_providers.json", json_req, miss_json_res));
  REQUIRE(miss_json_res.result() == http::status::not_found);

  const auto git_unreadable = dir / "git-unreadable.yaml";
  {
    std::ofstream out(git_unreadable);
    REQUIRE(out.is_open());
    out << "providers: []\n";
  }
  std::filesystem::permissions(git_unreadable,
                               std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);
  holder::test::EnvGuard unreadable_env("HOLDER_GIT_PROVIDERS_PATH", git_unreadable.string());
  http::response<http::string_body> miss_yaml_read_res;
  REQUIRE(holder::api::routes::handle_static_routes("/git_providers.yaml", yaml_req, miss_yaml_read_res));
  REQUIRE(miss_yaml_read_res.result() == http::status::not_found);
  http::response<http::string_body> miss_json_read_res;
  REQUIRE(holder::api::routes::handle_static_routes("/git_providers.json", json_req, miss_json_read_res));
  REQUIRE(miss_json_read_res.result() == http::status::not_found);

  const auto bad_yaml = dir / "bad-git.yaml";
  {
    std::ofstream out(bad_yaml);
    REQUIRE(out.is_open());
    out << "providers: [\n";
  }
  holder::test::EnvGuard bad_env("HOLDER_GIT_PROVIDERS_PATH", bad_yaml.string());
  http::response<http::string_body> bad_json_res;
  REQUIRE(holder::api::routes::handle_static_routes("/git_providers.json", json_req, bad_json_res));
  REQUIRE(bad_json_res.result() == http::status::internal_server_error);
}

TEST_CASE("StaticRoutes ai_catalog yaml handles missing and unreadable path", "[static-routes]") {
  const auto dir = holder::test::make_temp_dir();
  CwdGuard cwd(dir);
  auto req = make_request(http::verb::get, "/ai_catalog.yaml");

  holder::test::EnvGuard missing_env("HOLDER_AI_CATALOG_PATH", (dir / "missing-ai-yaml.yaml").string());
  http::response<http::string_body> missing_res;
  REQUIRE(holder::api::routes::handle_static_routes("/ai_catalog.yaml", req, missing_res));
  REQUIRE(missing_res.result() == http::status::not_found);

  const auto ai_unreadable = dir / "ai-catalog-unreadable.yaml";
  {
    std::ofstream out(ai_unreadable);
    REQUIRE(out.is_open());
    out << "x: 1\n";
  }
  std::filesystem::permissions(ai_unreadable,
                               std::filesystem::perms::none,
                               std::filesystem::perm_options::replace);
  holder::test::EnvGuard unreadable_env("HOLDER_AI_CATALOG_PATH", ai_unreadable.string());
  http::response<http::string_body> unreadable_res;
  REQUIRE(holder::api::routes::handle_static_routes("/ai_catalog.yaml", req, unreadable_res));
  REQUIRE(unreadable_res.result() == http::status::not_found);
}

TEST_CASE("StaticRoutes docs paths handle missing root, unsafe relpath and missing files", "[static-routes]") {
  const auto dir = holder::test::make_temp_dir();
  CwdGuard cwd(dir);

  auto docs_req = make_request(http::verb::get, "/docs");
  http::response<http::string_body> no_root_res;
  REQUIRE(holder::api::routes::handle_static_routes("/docs", docs_req, no_root_res));
  REQUIRE(no_root_res.result() == http::status::not_found);

  const auto docs_root = dir / "assets" / "swagger-ui";
  std::filesystem::create_directories(docs_root);
  {
    std::ofstream out(docs_root / "index.html");
    REQUIRE(out.is_open());
    out << "<h1>ok</h1>";
  }
  holder::test::EnvGuard docs_env("HOLDER_DOCS_ROOT", docs_root.string());

  auto docs_slash_req = make_request(http::verb::get, "/docs/");
  http::response<http::string_body> docs_slash_res;
  REQUIRE(holder::api::routes::handle_static_routes("/docs/", docs_slash_req, docs_slash_res));
  REQUIRE(docs_slash_res.result() == http::status::ok);

  auto unsafe_req = make_request(http::verb::get, "/docs/../secret");
  http::response<http::string_body> unsafe_res;
  REQUIRE(holder::api::routes::handle_static_routes("/docs/../secret", unsafe_req, unsafe_res));
  REQUIRE(unsafe_res.result() == http::status::not_found);

  auto missing_req = make_request(http::verb::get, "/docs/missing.js");
  http::response<http::string_body> missing_res;
  REQUIRE(holder::api::routes::handle_static_routes("/docs/missing.js", missing_req, missing_res));
  REQUIRE(missing_res.result() == http::status::not_found);
}
