#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/GitRepo.h"

#include <filesystem>
#include <fstream>
#include <chrono>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_git_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("GitRepo throws when not opened", "[git]") {
  holder::git::GitRepo repo;
  REQUIRE_THROWS(repo.write_file("a.txt", "data"));
  REQUIRE_THROWS(repo.stage_path("a.txt"));
  REQUIRE_THROWS(repo.commit("msg"));
  REQUIRE_THROWS(repo.set_remote("origin", "git@example.com:repo.git"));
  REQUIRE_THROWS(repo.remove_remote("origin"));
}

TEST_CASE("GitRepo open_or_init fails on file path", "[git]") {
  const auto dir = make_temp_dir();
  const auto file_path = dir / "not_a_dir";
  std::ofstream out(file_path);
  REQUIRE(out.is_open());
  out << "x";
  out.close();

  holder::git::GitRepo repo;
  REQUIRE_THROWS(repo.open_or_init(file_path));
}

TEST_CASE("GitRepo stage_path throws on missing file", "[git]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);

  REQUIRE_THROWS(repo.stage_path("missing.txt"));
}
