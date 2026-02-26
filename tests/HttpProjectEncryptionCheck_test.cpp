#include "http_test_helpers.h"

#include <fstream>

using holder::test::http_json_request;
using holder::test::make_temp_dir;
using holder::test::open_db_with_schema;
using holder::test::ensure_uuid_seeded;
using holder::test::EnvGuard;

namespace {

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  REQUIRE(out.is_open());
  out << text;
}

} // namespace

TEST_CASE("HTTP project privacy-check reports unsafe plaintext blobs", "[http]") {
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
  std::thread server_thread([&server, &signals]() { server.run(signals); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto project_root = dir / "repo";
  http_json_request(bound.bind,
                    bound.port,
                    token,
                    boost::beast::http::verb::post,
                    "/projects",
                    {{"project_id", "proj-1"},
                     {"name", "Project One"},
                     {"root_path", project_root.string()}},
                    boost::beast::http::status::created);

  write_file(project_root / "cards" / "ab" / "plain.md", "# hello\nworld\n");

  const auto checked = http_json_request(bound.bind,
                                         bound.port,
                                         token,
                                         boost::beast::http::verb::get,
                                         "/projects/proj-1/encryption-check",
                                         nlohmann::json::object(),
                                         boost::beast::http::status::ok);
  REQUIRE(checked["ok"] == true);
  REQUIRE(checked["data"]["privacy_mode"] == "encrypted_git");
  REQUIRE(checked["data"]["check"]["ok"] == false);
  REQUIRE(checked["data"]["check"]["unsafe_files"] == 1);
  REQUIRE(checked["data"]["check"]["unsafe_paths"].is_array());
  REQUIRE(checked["data"]["check"]["unsafe_paths"][0] == "cards/ab/plain.md");

  http_json_request(bound.bind,
                    bound.port,
                    token,
                    boost::beast::http::verb::patch,
                    "/projects/proj-1",
                    {{"privacy_mode", "plain"}, {"updated_at", 20}},
                    boost::beast::http::status::ok);

  const auto checked_plain = http_json_request(bound.bind,
                                               bound.port,
                                               token,
                                               boost::beast::http::verb::get,
                                               "/projects/proj-1/encryption-check",
                                               nlohmann::json::object(),
                                               boost::beast::http::status::ok);
  REQUIRE(checked_plain["ok"] == true);
  REQUIRE(checked_plain["data"]["privacy_mode"] == "plain");
  REQUIRE(checked_plain["data"]["check"]["ok"] == true);
  REQUIRE(checked_plain["data"]["check"]["checked_files"] == 0);
  REQUIRE(checked_plain["data"]["check"]["unsafe_files"] == 0);

  std::raise(SIGTERM);
  server_thread.join();
}
