#if __has_include(<catch2/catch_test_macros.hpp>)
#include <catch2/catch_test_macros.hpp>
#else
#include <catch2/catch.hpp>
#endif

#include "git/GitRepo.h"
#include "git/RepoSyncMetrics.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto suffix = std::to_string(
      static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
  auto dir = base / ("holder_repo_sync_metrics_test_" + suffix);
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

TEST_CASE("RepoSyncMetrics ignores .holder/privacy.json in uncommitted count", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);

  std::filesystem::create_directories(dir / ".holder");
  {
    std::ofstream privacy_file(dir / ".holder" / "privacy.json");
    REQUIRE(privacy_file.is_open());
    privacy_file << "{\"version\":1}";
  }
  {
    std::ofstream card_file(dir / "cards.md");
    REQUIRE(card_file.is_open());
    card_file << "card body";
  }

  const auto metrics = holder::git::inspect_repo_sync_metrics(dir);
  REQUIRE(metrics.uncommitted_changes_count == 1);
}

TEST_CASE("RepoSyncMetrics returns zero unpushed when remote-tracking ref is missing", "[git][sync]") {
  const auto dir = make_temp_dir();
  holder::git::GitRepo repo;
  repo.open_or_init(dir);
  repo.write_file("cards/a.md", "hello");
  repo.stage_path("cards/a.md");
  repo.commit("seed");

  const auto metrics = holder::git::inspect_repo_sync_metrics(dir);
  REQUIRE(metrics.unpushed_commits_count == 0);
}
