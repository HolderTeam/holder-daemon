#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "core/ServerInfo.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_serverinfo_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

bool is_hex(const std::string& s) {
  for (char c : s) {
    if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
  }
  return true;
}

} // namespace

TEST_CASE("generate_auth_token creates hex token", "[serverinfo]") {
  const auto token = holder::core::generate_auth_token();
  REQUIRE(token.size() == 32);
  REQUIRE(is_hex(token));
}

TEST_CASE("write_server_info writes JSON with expected fields", "[serverinfo]") {
  const auto dir = make_temp_dir();
  const auto info_path = dir / "holder.json";

  holder::core::ServerInfo info;
  info.pid = 42;
  info.bind = "127.0.0.1";
  info.port = 1234;
  info.started_at = 1700000000;
  info.api_version = "0.1";
  info.server_version = "0.1.0";
  info.auth_token = "deadbeef";

  holder::core::write_server_info(info_path, info);

  std::ifstream in(info_path);
  REQUIRE(in.is_open());
  nlohmann::json j;
  in >> j;

  REQUIRE(j["pid"] == 42);
  REQUIRE(j["bind"] == "127.0.0.1");
  REQUIRE(j["port"] == 1234);
  REQUIRE(j["started_at"] == 1700000000);
  REQUIRE(j["api_version"] == "0.1");
  REQUIRE(j["server_version"] == "0.1.0");
  REQUIRE(j["auth_token"] == "deadbeef");
}

TEST_CASE("write_server_info overwrites existing file", "[serverinfo]") {
  const auto dir = make_temp_dir();
  const auto info_path = dir / "holder.json";

  {
    std::ofstream out(info_path);
    out << "stale";
  }

  holder::core::ServerInfo info;
  info.pid = 7;
  info.bind = "127.0.0.1";
  info.port = 4321;
  info.started_at = 1700000001;
  info.api_version = "0.1";
  info.server_version = "0.1.1";
  info.auth_token = "beefdead";

  holder::core::write_server_info(info_path, info);

  std::ifstream in(info_path);
  REQUIRE(in.is_open());
  nlohmann::json j;
  in >> j;
  REQUIRE(j["pid"] == 7);
  REQUIRE(j["port"] == 4321);
  REQUIRE(j["auth_token"] == "beefdead");
}
