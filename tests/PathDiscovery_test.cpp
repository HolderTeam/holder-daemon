#include "api/support/PathDiscovery.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE(
    "PathDiscovery content_type_for_extension covers image/text and fallback mappings",
    "[path_discovery]"
) {
  using holder::api::support::content_type_for_extension;

  REQUIRE(content_type_for_extension(".svg") == "image/svg+xml");
  REQUIRE(content_type_for_extension(".png") == "image/png");
  REQUIRE(content_type_for_extension(".ico") == "image/x-icon");
  REQUIRE(content_type_for_extension(".txt") == "text/plain; charset=utf-8");
  REQUIRE(content_type_for_extension(".bin") == "application/octet-stream");
}
