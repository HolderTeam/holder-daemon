#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "api/routes/AiResourceRoutes.h"
#include "http_test_helpers.h"

#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>

#include <string>

namespace {
namespace http = boost::beast::http;

http::request<http::string_body> make_request(
    http::verb method,
    const std::string& target,
    const nlohmann::json& body = nlohmann::json()
) {
  http::request<http::string_body> req{method, target, 11};
  req.set(http::field::host, "127.0.0.1");
  if (!body.is_null() && !body.empty()) {
    req.set(http::field::content_type, "application/json");
    req.body() = body.dump();
    req.prepare_payload();
  }
  return req;
}

} // namespace

TEST_CASE(
    "AiResourceRoutes POST maps non-conflict exceptions to bad_request",
    "[resources][routes]"
) {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  // Force a non-conflict exception inside POST handling: type mismatch on project_id.
  auto req = make_request(
      http::verb::post,
      "/resources",
      nlohmann::json{
          {"project_id", nlohmann::json::array()},
          {"kind", "url"},
          {"uri", "https://example.com"},
          {"label", "Example"}
      }
  );
  http::response<http::string_body> res;

  const bool handled = holder::api::routes::handle_ai_resource_routes(
      "/resources",
      req,
      res,
      db,
      uuid_v4,
      param_get
  );
  REQUIRE(handled);
  REQUIRE(res.result() == http::status::bad_request);

  const auto payload = nlohmann::json::parse(res.body());
  REQUIRE(payload["ok"] == false);
  REQUIRE(payload["error"]["code"] == "bad_request");
}

TEST_CASE("AiResourceRoutes returns false for unrelated paths", "[resources][routes]") {
  const auto dir = holder::test::make_temp_dir();
  auto db = holder::test::open_db_with_schema(dir / "holder.db");

  const auto uuid_v4 = []() {
    return std::string("generated-id");
  };
  const auto param_get = [](const std::string&) {
    return std::string();
  };

  auto req = make_request(http::verb::get, "/not-a-resource-route");
  http::response<http::string_body> res;

  const bool handled = holder::api::routes::handle_ai_resource_routes(
      "/not-a-resource-route",
      req,
      res,
      db,
      uuid_v4,
      param_get
  );
  REQUIRE_FALSE(handled);
}

#include "git/GitOps.h"
#include "google_storage_test_helpers.h"
#include "resource/LocationBindingStore.h"
#include "resource/LocationRepo.h"
#include "resource/ResourceRepo.h"
#include "storage_http_test_server.h"

namespace {
struct LocationRouteFixture {
  std::filesystem::path root = holder::test::make_temp_dir();
  holder::test::EnvGuard cache{"XDG_CACHE_HOME", (root / "cache").string()};
  holder::platform::Db db = holder::test::open_db_with_schema(root / "holder.db");
  std::unique_ptr<holder::privacy::SecretStore> secrets =
      holder::privacy::make_encrypted_file_secret_store_for_tests(root / "secrets");
  unsigned sequence = 0;
  LocationRouteFixture() {
    holder::test::create_project(db, "project", (root / "project").string());
  }
  http::response<http::string_body> call(
      http::verb method,
      const std::string& path,
      nlohmann::json body = nlohmann::json::object(),
      std::string project = "project",
      bool with_secrets = true,
      const std::map<std::string, std::string>& params = {},
      holder::git::GitOps* git = nullptr
  ) {
    auto request = make_request(method, path, body);
    http::response<http::string_body> response;
    REQUIRE(holder::api::routes::handle_ai_resource_routes(
        path,
        request,
        response,
        db,
        [&] {
          return "id-" + std::to_string(++sequence);
        },
        [&](const std::string& key) {
          const auto found = params.find(key);
          if (found != params.end()) return found->second;
          return key == "project_id" ? project : "";
        },
        with_secrets ? secrets.get() : nullptr,
        git
    ));
    return response;
  }
  void put(
      const std::string& provider,
      std::map<std::string, std::string> configuration,
      std::map<std::string, std::string> values
  ) {
    holder::resource::LocationRepo(db).put(
        {"location", "project", "Storage", provider, std::move(configuration), 1, 1}
    );
    holder::resource::LocationBindingStore(*secrets)
        .bind("project", "location", {1, provider, std::move(values)}, "Configured", 1);
  }
};
} // namespace

TEST_CASE("Location routes manage bindings preferences and declarations", "[resources][routes]") {
  LocationRouteFixture f;
  auto created = f.call(
      http::verb::post,
      "/locations",
      {{"location_id", "location"},
       {"project_id", "project"},
       {"name", "Storage"},
       {"provider", "local_directory"}}
  );
  REQUIRE(created.result() == http::status::created);
  CHECK(
      f.call(
           http::verb::put,
           "/locations/location/binding",
           {{"values", {{"root_path", (f.root / "storage").string()}}}}
      ).result() == http::status::ok
  );
  CHECK(
      f.call(
           http::verb::put,
           "/locations/preferred",
           {{"project_id", "project"}, {"location_id", "location"}}
      ).result() == http::status::ok
  );
  auto details = f.call(http::verb::get, "/locations/location");
  CHECK(nlohmann::json::parse(details.body())["data"]["bound"] == true);
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::ok);
  CHECK(
      f.call(
           http::verb::patch,
           "/locations/location",
           {{"name", "Renamed"}, {"configuration", {{"prefix", "//archive//"}}}, {"updated_at", 22}}
      ).result() == http::status::ok
  );
  REQUIRE(holder::resource::LocationRepo(f.db).get("location").has_value());
  CHECK(holder::resource::LocationRepo(f.db).get("location")->name == "Renamed");
  CHECK(f.call(http::verb::delete_, "/locations/location/binding").result() == http::status::ok);
  CHECK_FALSE(holder::resource::LocationBindingStore(*f.secrets).preferred("project").has_value());
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::conflict);
  CHECK(
      f.call(
           http::verb::put,
           "/locations/preferred",
           {{"project_id", "project"}, {"location_id", "location"}}
      ).result() == http::status::ok
  );
  CHECK(f.call(http::verb::delete_, "/locations/location").result() == http::status::ok);
  CHECK_FALSE(holder::resource::LocationRepo(f.db).get("location").has_value());
  CHECK_FALSE(holder::resource::LocationBindingStore(*f.secrets).preferred("project").has_value());
}

TEST_CASE("Location routes validate missing services fields and records", "[resources][routes]") {
  LocationRouteFixture f;
  for (auto method : {http::verb::get, http::verb::patch, http::verb::delete_}) {
    CHECK(f.call(method, "/locations/missing").result() == http::status::not_found);
  }
  for (auto method : {http::verb::put, http::verb::delete_}) {
    CHECK(f.call(method, "/locations/missing/binding").result() == http::status::not_found);
    CHECK(
        f.call(method, "/locations/missing/binding", {}, "project", false).result() ==
        http::status::bad_request
    );
  }
  CHECK(f.call(http::verb::post, "/locations/missing/test").result() == http::status::not_found);
  CHECK(
      f.call(http::verb::post, "/locations/missing/test", {}, "project", false).result() ==
      http::status::bad_request
  );
  CHECK(f.call(http::verb::get, "/locations", {}, "").result() == http::status::bad_request);
  CHECK(
      f.call(http::verb::post, "/locations", {{"name", "Missing fields"}}).result() ==
      http::status::bad_request
  );
  CHECK(
      f.call(
           http::verb::post,
           "/locations",
           {{"name", "Bad"}, {"project_id", "project"}, {"provider", "unsupported"}}
      ).result() == http::status::bad_request
  );
  CHECK(
      f.call(
           http::verb::put,
           "/locations/preferred",
           {{"project_id", "project"}, {"location_id", "missing"}}
      ).result() == http::status::not_found
  );
  CHECK(f.call(http::verb::post, "/imports").result() == http::status::bad_request);
  CHECK(f.call(http::verb::get, "/imports/missing").result() == http::status::not_found);
  for (const std::string path :
       {"/locations/", "/locations/id/unknown", "/resources/", "/resources/id/unknown"})
    CHECK(f.call(http::verb::get, path).result() == http::status::not_found);
}

TEST_CASE(
    "Location probes exercise S3 credentials and clean up after provider failures",
    "[resources][routes]"
) {
  using Server = holder::test::StorageHttpTestServer;
  unsigned response_status = 200;
  http::status expected = http::status::ok;
  SECTION("success") {}
  SECTION("authentication") {
    response_status = 401;
    expected = http::status::bad_gateway;
  }
  SECTION("permission") {
    response_status = 403;
    expected = http::status::bad_gateway;
  }
  SECTION("integrity") {
    response_status = 404;
    expected = http::status::service_unavailable;
  }
  SECTION("transient") {
    response_status = 503;
    expected = http::status::service_unavailable;
  }
  LocationRouteFixture f;
  Server server([&](const Server::Request&) {
    return Server::response(response_status);
  });
  f.put(
      "s3_compatible",
      {{"endpoint", server.endpoint()},
       {"region", "region"},
       {"bucket", "bucket"},
       {"allow_insecure_localhost", "true"},
       {"addressing_style", "path"},
       {"prefix", "//archive//"}},
      {{"access_key_id", "access"}, {"secret_access_key", "secret"}, {"session_token", "session"}}
  );
  const auto response = f.call(http::verb::post, "/locations/location/test");
  INFO(response.body());
  CHECK(response.result() == expected);
  server.stop();
  server.rethrow_error();
  REQUIRE_FALSE(server.requests().empty());
  CHECK(std::string(server.requests()[0].target()).starts_with("/bucket/archive/.holder-probes/"));
  CHECK(server.requests()[0]["x-amz-security-token"] == "session");
  CHECK(std::filesystem::is_empty(f.root / "cache/holder/asset-probes"));
}

TEST_CASE(
    "Location probes report invalid provider configuration before transferring",
    "[resources][routes]"
) {
  LocationRouteFixture f;
  f.put("s3_compatible", {}, {});
  for (const auto& [key, value] : std::map<std::string, std::string>{
           {"endpoint", "https://storage.invalid"},
           {"region", "region"},
           {"bucket", "bucket"}
       }) {
    CHECK(
        f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request
    );
    auto location = *holder::resource::LocationRepo(f.db).get("location");
    location.configuration[key] = value;
    holder::resource::LocationRepo(f.db).put(location);
  }
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request);
  f.put(
      "s3_compatible",
      {{"region", "region"}, {"bucket", "bucket"}},
      {{"endpoint", "https://storage.invalid"}, {"access_key_id", "access"}}
  );
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request);
  f.put("google-drive", {}, {});
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request);
  f.put("google-drive", {{"folder_id", "folder"}}, {});
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request);
  f.put("local_directory", {}, {});
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request);
  f.put("unsupported", {}, {});
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request);
  f.put("local_directory", {}, {{"root_path", f.root.string()}});
  holder::resource::LocationBindingStore(*f.secrets)
      .bind("project", "location", {1, "google-drive", {}}, "Mismatch", 1);
  CHECK(f.call(http::verb::post, "/locations/location/test").result() == http::status::bad_request);
}

TEST_CASE("Location probes use Google OAuth and remove the probe object", "[resources][routes]") {
  using Server = holder::test::StorageHttpTestServer;
  LocationRouteFixture f;
  holder::test::EnvGuard client("HOLDER_GOOGLE_OAUTH_CLIENT_ID", "client");
  holder::test::EnvGuard secret("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", "secret");
  bool present = false;
  holder::test::GoogleStorageTestServer google([&](const Server::Request& request) {
    if (request.target() == "/token") return Server::response(200, R"({"access_token":"access"})");
    if (request.method() == http::verb::get)
      return Server::response(200, present ? R"({"files":[{"id":"probe"}]})" : R"({"files":[]})");
    if (request.method() == http::verb::post) {
      present = true;
      return Server::response(200, R"({"id":"probe"})");
    }
    present = false;
    return Server::response(204);
  });
  f.put("google-drive", {{"folder_id", "folder"}}, {{"refresh_token", "refresh"}});
  const auto response = f.call(http::verb::post, "/locations/location/test");
  INFO(response.body());
  CHECK(response.result() == http::status::ok);
  google.finish();
  CHECK_FALSE(present);
  CHECK(std::filesystem::is_empty(f.root / "cache/holder/asset-probes"));
}

#include "resource/StorageProvider.h"

TEST_CASE("Resource errors preserve storage categories in HTTP responses", "[resources][routes]") {
  using Code = holder::resource::StorageErrorCode;
  struct Case {
    Code code;
    unsigned status;
    const char* name;
  };
  for (const auto& item : std::vector<Case>{
           {Code::Unavailable, 503, "storage_unavailable"},
           {Code::Authentication, 502, "storage_authentication_failed"},
           {Code::Permission, 502, "storage_permission_denied"},
           {Code::Capacity, 507, "storage_capacity_exceeded"},
           {Code::Integrity, 422, "storage_integrity_failed"},
           {Code::Conflict, 409, "storage_conflict"},
           {Code::InvalidConfiguration, 400, "storage_configuration_invalid"},
           {Code::Transient, 503, "storage_transient_failure"}
       }) {
    CAPTURE(item.name);
    const auto response = holder::api::routes::resource_error_response(
        holder::resource::StorageError(item.code, "storage failure")
    );
    CHECK(response.result_int() == item.status);
    const auto body = nlohmann::json::parse(response.body());
    CHECK(body["error"]["code"] == item.name);
    CHECK(body["error"]["message"] == "storage failure");
  }
  const auto conflict = holder::api::routes::resource_error_response(
      std::runtime_error("conflict: resource already exists")
  );
  CHECK(conflict.result() == http::status::conflict);
}

TEST_CASE("Resource routes validate updates and report database failures", "[resources][routes]") {
  LocationRouteFixture f;
  CHECK(
      f.call(http::verb::get, "/resources", {}, "project", true, {{"limit", "1"}}).result() ==
      http::status::bad_request
  );
  CHECK(
      f.call(
           http::verb::post,
           "/resources",
           {{"project_id", 42}, {"type", "url"}, {"label", "Broken"}}
      ).result() == http::status::bad_request
  );
  CHECK(
      f.call(http::verb::get, "/resources/resource/assets/asset/content").result() ==
      http::status::bad_request
  );
  REQUIRE(
      f.call(
           http::verb::post,
           "/resources",
           {{"project_id", "project"},
            {"resource_id", "resource"},
            {"type", "url"},
            {"label", "Example"}}
      ).result() == http::status::created
  );
  CHECK(
      f.call(http::verb::patch, "/resources/missing", {{"label", "New"}}).result() ==
      http::status::not_found
  );
  CHECK(
      f.call(http::verb::patch, "/resources/resource", {{"label", 42}}).result() ==
      http::status::bad_request
  );
  REQUIRE(
      f.call(http::verb::patch, "/resources/resource", {{"label", "Changed"}}).result() ==
      http::status::ok
  );
  CHECK(holder::resource::ResourceRepo(f.db).get_bundle("resource")->resource.label == "Changed");
  CHECK(f.call(http::verb::put, "/resources/resource").result() == http::status::not_found);
  REQUIRE(f.call(http::verb::delete_, "/resources/resource").result() == http::status::ok);
  CHECK_FALSE(holder::resource::ResourceRepo(f.db).get_bundle("resource").has_value());
  f.db.exec("DROP TABLE resources; DROP TABLE storage_locations");
  CHECK(f.call(http::verb::delete_, "/resources/resource").result() == http::status::bad_request);
  CHECK(f.call(http::verb::get, "/locations").result() == http::status::bad_request);
}

TEST_CASE("Location updates and removals persist their Git manifests", "[resources][routes]") {
  LocationRouteFixture f;
  holder::git::RealGitOps git;
  f.put("local_directory", {}, {{"root_path", f.root.string()}});
  REQUIRE(
      f.call(
           http::verb::patch,
           "/locations/location",
           {{"name", "Changed"}},
           "project",
           true,
           {},
           &git
      )
          .result() == http::status::ok
  );
  CHECK(holder::resource::LocationRepo(f.db).get("location")->name == "Changed");
  REQUIRE(
      f.call(http::verb::delete_, "/locations/location", {}, "project", true, {}, &git).result() ==
      http::status::ok
  );
  CHECK_FALSE(holder::resource::LocationRepo(f.db).get("location").has_value());
}
