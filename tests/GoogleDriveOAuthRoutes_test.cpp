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
  REQUIRE(response["error"]["message"].get<std::string>().find("HOLDER_GOOGLE_OAUTH_CLIENT_ID")
          != std::string::npos);
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
      "/locations/" + location_id +
          "/oauth/google-drive/callback?state=not-" + real_state + "&code=some-code"
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
      "/locations/" + location_id +
          "/oauth/google-drive/callback?state=" + real_state + "&error=access_denied"
  );
  REQUIRE(response.status == boost::beast::http::status::ok);
  REQUIRE(response.body.find("cancelled") != std::string::npos);
}
