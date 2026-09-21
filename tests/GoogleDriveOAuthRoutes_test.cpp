#include "http_test_helpers.h"

using holder::test::create_project;
using holder::test::EnvGuard;
using holder::test::http_json_request;
using holder::test::http_request_raw;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

namespace {

struct RunningServer {
  holder::index::FtsIndexer fts;
  holder::card::CardStore cards;
  holder::api::HttpServer server;
  holder::core::SignalHandler signals;
  holder::api::HttpServer::BoundInfo bound;
  std::unique_ptr<holder::test::HttpServerThreadGuard> thread;

  RunningServer(holder::platform::Db& db, const std::string& token)
      : fts(db),
        cards(db, &fts),
        server("127.0.0.1", 0, db, token, &cards, &fts) {
    try {
      bound = server.start();
    } catch (const std::exception& ex) {
      SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
    }
    thread = std::make_unique<holder::test::HttpServerThreadGuard>(server, signals);
    REQUIRE(holder::test::wait_for_http_listener(bound.bind, bound.port));
  }
};

std::string create_google_drive_location(
    const holder::api::HttpServer::BoundInfo& bound,
    const std::string& token,
    const std::string& project_id
) {
  const auto created = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations",
      {{"project_id", project_id}, {"name", "My Google Drive"}, {"provider", "google-drive"}},
      boost::beast::http::status::created
  );
  return created["data"]["location_id"].get<std::string>();
}

std::string extract_query_value(const std::string& url, const std::string& key) {
  const auto needle = key + "=";
  const auto pos = url.find(needle);
  REQUIRE(pos != std::string::npos);
  const auto start = pos + needle.size();
  const auto end = url.find('&', start);
  return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

} // namespace

TEST_CASE(
    "POST /locations/{id}/oauth/google-drive/authorize returns an authorization URL",
    "[http][resources][google_drive]"
) {
  EnvGuard client_id_guard("HOLDER_GOOGLE_OAUTH_CLIENT_ID", "test-client-id");
  EnvGuard client_secret_guard("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", "test-client-secret");

  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);

  const auto location_id = create_google_drive_location(running.bound, token, "proj-1");

  const auto started = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations/" + location_id + "/oauth/google-drive/authorize",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  const auto url = started["data"]["authorization_url"].get<std::string>();
  REQUIRE(url.rfind("https://accounts.google.com/o/oauth2/v2/auth?", 0) == 0);
  REQUIRE(url.find("client_id=test-client-id") != std::string::npos);
  REQUIRE(url.find("code_challenge=") != std::string::npos);
  REQUIRE(url.find("state=") != std::string::npos);
  // Never leaked into the URL the user's browser (and its history) sees.
  REQUIRE(url.find("test-client-secret") == std::string::npos);
}

TEST_CASE(
    "POST .../authorize rejects a location that isn't a google-drive provider",
    "[http][resources][google_drive]"
) {
  EnvGuard client_id_guard("HOLDER_GOOGLE_OAUTH_CLIENT_ID", "test-client-id");
  EnvGuard client_secret_guard("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", "test-client-secret");

  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);

  const auto created = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations",
      {{"project_id", "proj-1"}, {"name", "Local"}, {"provider", "local_directory"}},
      boost::beast::http::status::created
  );
  const auto location_id = created["data"]["location_id"].get<std::string>();

  http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations/" + location_id + "/oauth/google-drive/authorize",
      nlohmann::json::object(),
      boost::beast::http::status::bad_request
  );
}

TEST_CASE(
    "POST .../authorize fails clearly when the daemon has no OAuth client configured",
    "[http][resources][google_drive]"
) {
  // Deliberately no EnvGuard here -- simulates a daemon that was never given
  // HOLDER_GOOGLE_OAUTH_CLIENT_ID/_SECRET.
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);

  const auto location_id = create_google_drive_location(running.bound, token, "proj-1");

  const auto response = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations/" + location_id + "/oauth/google-drive/authorize",
      nlohmann::json::object(),
      boost::beast::http::status::bad_request
  );
  REQUIRE(
      response["error"]["message"].get<std::string>().find("HOLDER_GOOGLE_OAUTH_CLIENT_ID") !=
      std::string::npos
  );
}

TEST_CASE(
    "GET .../callback with no matching pending attempt reports the connection expired, "
    "without requiring auth",
    "[http][resources][google_drive]"
) {
  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  const std::string token = "testtoken";
  RunningServer running(db, token);

  // No token at all -- this is the point of the route: Google's browser redirect never
  // carries the daemon's bearer token.
  const auto response = http_request_raw(
      running.bound.bind,
      running.bound.port,
      "",
      boost::beast::http::verb::get,
      "/locations/no-such-location/oauth/google-drive/callback?state=x&code=y"
  );
  REQUIRE(response.status == boost::beast::http::status::bad_request);
  REQUIRE(response.content_type.find("text/html") != std::string::npos);
  REQUIRE(response.body.find("Connection expired") != std::string::npos);
}

TEST_CASE(
    "GET .../callback rejects a state that doesn't match the pending attempt",
    "[http][resources][google_drive]"
) {
  EnvGuard client_id_guard("HOLDER_GOOGLE_OAUTH_CLIENT_ID", "test-client-id");
  EnvGuard client_secret_guard("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", "test-client-secret");

  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);

  const auto location_id = create_google_drive_location(running.bound, token, "proj-1");
  const auto started = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations/" + location_id + "/oauth/google-drive/authorize",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  const auto real_state = extract_query_value(started["data"]["authorization_url"], "state");
  REQUIRE_FALSE(real_state.empty());

  // No token here either -- same reasoning as the previous test.
  const auto response = http_request_raw(
      running.bound.bind,
      running.bound.port,
      "",
      boost::beast::http::verb::get,
      "/locations/" + location_id + "/oauth/google-drive/callback?state=not-" + real_state +
          "&code=some-code"
  );
  REQUIRE(response.status == boost::beast::http::status::bad_request);
  REQUIRE(response.body.find("Connection failed") != std::string::npos);
}

TEST_CASE(
    "GET .../callback reports cancellation when Google reports an error and state matches",
    "[http][resources][google_drive]"
) {
  EnvGuard client_id_guard("HOLDER_GOOGLE_OAUTH_CLIENT_ID", "test-client-id");
  EnvGuard client_secret_guard("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", "test-client-secret");

  const auto dir = make_temp_dir();
  auto db = open_db_with_schema(dir / "holder.db");
  create_project(db, "proj-1", (dir / "project_repo").string());
  const std::string token = "testtoken";
  RunningServer running(db, token);

  const auto location_id = create_google_drive_location(running.bound, token, "proj-1");
  const auto started = http_json_request(
      running.bound.bind,
      running.bound.port,
      token,
      boost::beast::http::verb::post,
      "/locations/" + location_id + "/oauth/google-drive/authorize",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  const auto real_state = extract_query_value(started["data"]["authorization_url"], "state");

  const auto response = http_request_raw(
      running.bound.bind,
      running.bound.port,
      "",
      boost::beast::http::verb::get,
      "/locations/" + location_id + "/oauth/google-drive/callback?state=" + real_state +
          "&error=access_denied"
  );
  REQUIRE(response.status == boost::beast::http::status::ok);
  REQUIRE(response.body.find("cancelled") != std::string::npos);
}

#include "api/routes/GoogleDriveOAuthRoutes.h"
#include "google_storage_test_helpers.h"
#include "resource/LocationBindingStore.h"
#include "resource/LocationRepo.h"

TEST_CASE(
    "Google Drive callback persists credentials and rejects incomplete exchanges",
    "[google_drive][routes]"
) {
  namespace http = boost::beast::http;
  using Server = holder::test::StorageHttpTestServer;
  namespace routes = holder::api::routes;
  const auto root = make_temp_dir();
  auto db = open_db_with_schema(root / "holder.db");
  create_project(db, "project", (root / "project").string());
  holder::model::Location location{"location", "project", "Drive", "google-drive", {}, 1, 1};
  holder::resource::LocationRepo(db).put(location);
  auto secrets = holder::privacy::make_encrypted_file_secret_store_for_tests(root / "secrets");
  EnvGuard client_id("HOLDER_GOOGLE_OAUTH_CLIENT_ID", "client");
  EnvGuard client_secret("HOLDER_GOOGLE_OAUTH_CLIENT_SECRET", "secret");
  std::string token_body = R"({"access_token":"access","refresh_token":"private-refresh"})";
  bool missing_code = false;
  bool missing_secrets = false;
  bool removed_location = false;
  bool missing_refresh = false;
  bool rejected_token = false;
  SECTION("successful exchange") {}
  SECTION("missing code") { missing_code = true; }
  SECTION("unavailable secret store") { missing_secrets = true; }
  SECTION("location removed during authorization") { removed_location = true; }
  SECTION("missing refresh token") {
    token_body = R"({"access_token":"access"})";
    missing_refresh = true;
  }
  SECTION("token endpoint rejection") { rejected_token = true; }
  holder::test::GoogleStorageTestServer fixture([&](const Server::Request& request) {
    if (request.target() == "/token")
      return Server::response(rejected_token ? 400 : 200, token_body);
    return Server::response(200, R"({"files":[{"id":"resources-folder"}]})");
  });
  http::request<http::string_body> request{http::verb::post, "/", 11};
  http::response<http::string_body> response;
  REQUIRE(routes::handle_google_drive_oauth_authorize_route("location", request, response, db));
  REQUIRE(response.result() == http::status::ok);
  const auto authorization =
      nlohmann::json::parse(response.body())["data"]["authorization_url"].get<std::string>();
  CHECK(extract_query_value(authorization, "redirect_uri").find("127.0.0.1") != std::string::npos);
  const auto state = extract_query_value(authorization, "state");
  if (removed_location) holder::resource::LocationRepo(db).remove("location");
  request.method(http::verb::get);
  const auto query = "state=" + state + (missing_code ? "" : "&code=code%2Bwith+space%invalid");
  REQUIRE(routes::handle_google_drive_oauth_callback_route(
      "/locations/location/oauth/google-drive/callback",
      query,
      request,
      response,
      db,
      missing_secrets ? nullptr : secrets.get(),
      nullptr
  ));
  const bool success = !(
      missing_code || missing_secrets || removed_location || missing_refresh || rejected_token
  );
  CHECK(
      response.result() == (success        ? http::status::ok
                            : missing_code ? http::status::bad_request
                                           : http::status::internal_server_error)
  );
  CHECK(response.body().find("private-refresh") == std::string::npos);
  const auto binding = holder::resource::LocationBindingStore(*secrets).get("project", "location");
  if (success) {
    REQUIRE(binding.has_value());
    CHECK(binding->values.at("refresh_token") == "private-refresh");
    REQUIRE(holder::resource::LocationRepo(db).get("location").has_value());
    CHECK(
        holder::resource::LocationRepo(db).get("location")->configuration.at("folder_id") ==
        "resources-folder"
    );
  } else
    CHECK_FALSE(binding.has_value());
  REQUIRE(routes::handle_google_drive_oauth_callback_route(
      "/locations/location/oauth/google-drive/callback",
      query,
      request,
      response,
      db,
      secrets.get(),
      nullptr
  ));
  CHECK(response.result() == http::status::bad_request);
  CHECK(response.body().find("Connection expired") != std::string::npos);
  fixture.finish();
  if (!missing_code && !missing_secrets) {
    REQUIRE_FALSE(fixture.server.requests().empty());
    CHECK(
        fixture.server.requests()[0].body().find("code=code%2Bwith%20space%25invalid") !=
        std::string::npos
    );
  }
}

TEST_CASE(
    "Google Drive OAuth handlers ignore unrelated callbacks and missing locations",
    "[google_drive][routes]"
) {
  namespace http = boost::beast::http;
  namespace routes = holder::api::routes;
  auto db = open_db_with_schema(make_temp_dir() / "holder.db");
  http::request<http::string_body> request{http::verb::get, "/", 11};
  http::response<http::string_body> response;
  for (const std::string path :
       {"/",
        "/locations/oauth/google-drive/callback",
        "/locations/location/oauth/google-drive/wrong"}) {
    CHECK_FALSE(routes::handle_google_drive_oauth_callback_route(
        path,
        "",
        request,
        response,
        db,
        nullptr,
        nullptr
    ));
  }
  request.method(http::verb::post);
  CHECK_FALSE(routes::handle_google_drive_oauth_callback_route(
      "/locations/location/oauth/google-drive/callback",
      "",
      request,
      response,
      db,
      nullptr,
      nullptr
  ));
  CHECK(routes::handle_google_drive_oauth_authorize_route("missing", request, response, db));
  CHECK(response.result() == http::status::bad_request);
}
