#include "http_test_helpers.h"

using holder::test::ensure_uuid_seeded;
using holder::test::EnvGuard;
using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;

TEST_CASE("HTTP project recovery token export/import round-trip", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);
  ensure_uuid_seeded();

  const auto projects_root = dir / "projects_root";
  std::filesystem::create_directories(projects_root);
  EnvGuard root_env("HOLDER_PROJECTS_ROOT", projects_root.string());

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto created = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects",
      {{"project_id", "proj-1"}, {"name", "Project One"}},
      boost::beast::http::status::created
  );
  REQUIRE(created["ok"] == true);

  const auto fetched = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/projects/proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(fetched["data"]["project_key_id"].is_string());
  const std::string key_id = fetched["data"]["project_key_id"].get<std::string>();
  REQUIRE_FALSE(key_id.empty());

  const auto exported = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects/proj-1/recovery-token/export",
      {{"pin", "1234"}},
      boost::beast::http::status::ok
  );
  REQUIRE(exported["ok"] == true);
  REQUIRE(exported["data"]["project_id"] == "proj-1");
  REQUIRE(exported["data"]["key_id"] == key_id);
  REQUIRE(exported["data"]["recovery_token"].is_string());
  const std::string recovery_token = exported["data"]["recovery_token"].get<std::string>();
  REQUIRE_FALSE(recovery_token.empty());

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::patch,
      "/projects/proj-1",
      {{"privacy_mode", "plain"}, {"project_key_id", nullptr}, {"updated_at", 30}},
      boost::beast::http::status::ok
  );

  const auto fetched_cleared = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/projects/proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(fetched_cleared["data"]["project_key_id"].is_null());

  const auto bad_import = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects/proj-1/recovery-token/import",
      {{"pin", "wrong"}, {"recovery_token", recovery_token}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(bad_import["ok"] == false);
  REQUIRE(bad_import["error"]["code"] == "privacy_recovery_token_invalid");

  const auto imported = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects/proj-1/recovery-token/import",
      {{"pin", "1234"}, {"recovery_token", recovery_token}},
      boost::beast::http::status::ok
  );
  REQUIRE(imported["ok"] == true);
  REQUIRE(imported["data"]["project_id"] == "proj-1");

  const auto fetched_after_import = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/projects/proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(fetched_after_import["data"]["project_key_id"] == key_id);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP recovery token import rejects project mismatch", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);
  ensure_uuid_seeded();

  const auto projects_root = dir / "projects_root";
  std::filesystem::create_directories(projects_root);
  EnvGuard root_env("HOLDER_PROJECTS_ROOT", projects_root.string());

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects",
      {{"project_id", "proj-1"}, {"name", "Project One"}},
      boost::beast::http::status::created
  );
  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects",
      {{"project_id", "proj-2"}, {"name", "Project Two"}},
      boost::beast::http::status::created
  );

  const auto exported = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects/proj-1/recovery-token/export",
      {{"pin", "1234"}},
      boost::beast::http::status::ok
  );
  const std::string recovery_token = exported["data"]["recovery_token"].get<std::string>();

  const auto mismatch = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects/proj-2/recovery-token/import",
      {{"pin", "1234"}, {"recovery_token", recovery_token}},
      boost::beast::http::status::bad_request
  );
  REQUIRE(mismatch["ok"] == false);

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP global recovery token import auto-creates project by token project_id", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);
  ensure_uuid_seeded();

  const auto projects_root = dir / "projects_root";
  std::filesystem::create_directories(projects_root);
  EnvGuard root_env("HOLDER_PROJECTS_ROOT", projects_root.string());

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects",
      {{"project_id", "proj-1"},
       {"name", "Project One"},
       {"git_remote_url", "https://example.com/recovered.git"}},
      boost::beast::http::status::created
  );

  const auto exported = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects/proj-1/recovery-token/export",
      {{"pin", "1234"}},
      boost::beast::http::status::ok
  );
  const std::string recovery_token = exported["data"]["recovery_token"].get<std::string>();
  const std::string key_id = exported["data"]["key_id"].get<std::string>();

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::delete_,
      "/projects/proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );

  const auto imported = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/recovery-token/import",
      {{"pin", "1234"}, {"recovery_token", recovery_token}},
      boost::beast::http::status::created
  );
  REQUIRE(imported["ok"] == true);
  REQUIRE(imported["data"]["project_id"] == "proj-1");
  REQUIRE(imported["data"]["project_created"] == true);
  REQUIRE(imported["data"]["remote_hint_present"] == true);
  REQUIRE(imported["data"]["remote_configured"] == true);
  REQUIRE(imported["data"]["remote_error"].is_null());
  REQUIRE(imported["data"]["pull_status"] == "failed");
  REQUIRE(imported["data"]["pull_error"].is_string());

  const auto fetched = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::get,
      "/projects/proj-1",
      nlohmann::json::object(),
      boost::beast::http::status::ok
  );
  REQUIRE(fetched["ok"] == true);
  REQUIRE(fetched["data"]["project_id"] == "proj-1");
  REQUIRE(fetched["data"]["project_key_id"] == key_id);
  REQUIRE(fetched["data"]["git_remote_url"] == "https://example.com/recovered.git");

  std::raise(SIGTERM);
  server_thread.join();
}

TEST_CASE("HTTP global recovery token import matches existing project_id", "[http]") {
  const auto dir = make_temp_dir();
  const auto db_path = dir / "holder.db";
  auto db = open_db_with_schema(db_path);
  ensure_uuid_seeded();

  const auto projects_root = dir / "projects_root";
  std::filesystem::create_directories(projects_root);
  EnvGuard root_env("HOLDER_PROJECTS_ROOT", projects_root.string());

  const std::string token = "testtoken";
  holder::api::HttpServer server("127.0.0.1", 0, db, token, nullptr, nullptr);
  holder::api::HttpServer::BoundInfo bound;
  try {
    bound = server.start();
  } catch (const std::exception& ex) {
    SKIP(std::string("Socket bind not available in test environment: ") + ex.what());
  }

  holder::core::SignalHandler signals;
  std::thread server_thread([&server, &signals]() {
    server.run(signals);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects",
      {{"project_id", "proj-1"}, {"name", "Project One"}},
      boost::beast::http::status::created
  );

  const auto exported = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/projects/proj-1/recovery-token/export",
      {{"pin", "1234"}},
      boost::beast::http::status::ok
  );
  const std::string recovery_token = exported["data"]["recovery_token"].get<std::string>();

  const auto imported = http_json_request(
      bound.bind,
      bound.port,
      token,
      boost::beast::http::verb::post,
      "/recovery-token/import",
      {{"pin", "1234"}, {"recovery_token", recovery_token}},
      boost::beast::http::status::ok
  );
  REQUIRE(imported["ok"] == true);
  REQUIRE(imported["data"]["project_id"] == "proj-1");
  REQUIRE(imported["data"]["project_created"] == false);
  REQUIRE(imported["data"]["remote_hint_present"] == false);
  REQUIRE(imported["data"]["remote_configured"] == false);
  REQUIRE(imported["data"]["remote_error"].is_null());
  REQUIRE(imported["data"]["pull_status"] == "not_attempted");
  REQUIRE(imported["data"]["pull_error"].is_null());

  std::raise(SIGTERM);
  server_thread.join();
}
