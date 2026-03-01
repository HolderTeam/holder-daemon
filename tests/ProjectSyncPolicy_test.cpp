#include "sync/ProjectSyncPolicy.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("ProjectSyncPolicy allows first push when never pushed", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_push(
      {.last_push_at = std::nullopt, .next_retry_at = std::nullopt, .now = 1000, .push_interval_seconds = 1200});
  REQUIRE(decision);
}

TEST_CASE("ProjectSyncPolicy blocks push before retry window", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_push(
      {.last_push_at = 1000, .next_retry_at = 1400, .now = 1200, .push_interval_seconds = 1200});
  REQUIRE_FALSE(decision);
}

TEST_CASE("ProjectSyncPolicy allows push after interval and retry gate", "[sync][policy]") {
  const bool decision = holder::sync::should_attempt_push(
      {.last_push_at = 1000, .next_retry_at = 1300, .now = 2500, .push_interval_seconds = 1200});
  REQUIRE(decision);
}

